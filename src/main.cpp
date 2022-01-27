#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <Arduino.h>
#include <EEPROM.h>

extern "C" {
#include "user_interface.h"
}


// WiFi credentials
#define CREDS_MAGIC 14337
struct creds {
    uint16_t crc;
    char pass[64];  // max length of wifi password is 63 bytes
    char ssid[33];  // max length of wifi ssid is 32 bytes
} creds;

// firmware update password <ip>:8080/firmware
const char* fwpw = "AHU_8266";

// rtc user memory storage: one integer
#define RTC_BASE 32
struct rtc_storage {
    uint8_t magic[3];  // RUM
    uint8_t checksum;  // sum(data_bytes)
    union {
        int32_t data;
        uint8_t data_bytes[4];
    };
} __attribute__((packed, aligned(4)));

// servers
WiFiServer *server = nullptr;
ESP8266WebServer httpServer(8080);
ESP8266HTTPUpdateServer httpUpdater;

// clients
#define MAX_CLIENTS 16
WiFiClient *client[MAX_CLIENTS] = { nullptr };

// stats buffer
char buffer[2048];

// at commands buffer
char input_buffer[2048];
int input_pos = 0;

// at+cipsend buffer
char send_buffer[2048];
int send_len = 0;
int send_pos = 0;
int send_to = -1;

// history
#define HISTSIZE 8
struct history {
    char buffer[128];
    struct history *next;
} *history, h[HISTSIZE];


// rtc user memory write
bool rtc_usermem_set(int32_t data)
{
    struct rtc_storage d = {
        .magic = {'R','U','M'},
    };

    d.data = data;
    d.checksum = d.data_bytes[0] + d.data_bytes[1] + \
                 d.data_bytes[2] + d.data_bytes[3];

    return system_rtc_mem_write(RTC_BASE, &d, sizeof(d));
}


// rtc user memory read
bool rtc_usermem_get(int32_t *data)
{
    struct rtc_storage d;
    uint8_t csum;

    if (!system_rtc_mem_read(RTC_BASE, &d, sizeof(d)))
        return false;

    if (d.magic[0] != 'R' || d.magic[1] != 'U' || d.magic[2] != 'M')
        return false;

    csum = d.data_bytes[0] + d.data_bytes[1] + \
           d.data_bytes[2] + d.data_bytes[3];

    if (csum != d.checksum)
        return false;

    *data = d.data;

    return true;
}


// 64bit millis() implementation
uint64_t millis64(void)
{
    static uint32_t low32 = 0, high32 = 0;
    uint32_t new_low32 = millis();
    if (new_low32 < low32) high32++;
    low32 = new_low32;
    return (uint64_t) high32 << 32 | low32;
}


// calculate crc for ssid + password
uint16_t creds_crc(struct creds *creds)
{
    uint16_t crc = 0;
    unsigned i;

    for (i = 0; i < sizeof(creds->ssid); i++)
        crc += (uint8_t)creds->ssid[i];

    for (i = 0; i < sizeof(creds->pass); i++)
        crc += (uint8_t)creds->pass[i];
        
    return crc + CREDS_MAGIC;
}


// close all client connections and stop server
void stop_server()
{
    // close all client connections...
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client[i] == nullptr)
            continue;

        client[i]->stop();
        delete client[i];
        client[i] = nullptr;
    }

    // stop server
    if (server != nullptr) {
        server->stop();
        delete server;
        server = nullptr;
    }

    // update port in rtc user memory
    rtc_usermem_set(0);
}


// process AT command
void process_command(char *command, size_t len)
{
    if (len == 0)
        return;

    history = history->next;

    if (len < sizeof(history->buffer)) {
        memcpy(history->buffer, command, len);
        history->buffer[len] = '\0';
    } else {
        memcpy(history->buffer, command, sizeof(history->buffer) - 1);
        history->buffer[sizeof(history->buffer) - 1] = '\0';
    }

    // test
    if (!strcmp(command, "AT"))
        goto ok;

    // reset
    if (!strcmp(command, "AT+RST")) {
        stop_server();
        Serial.print(F("\r\nOK\r\n...bla-bla-bla...\r\nready\r\n"));
        return;
    }

    // station mode
    if (!strcmp(command, "AT+CWMODE=1"))
        goto ok;

    // enable multiple connections
    if (!strcmp(command, "AT+CIPMUX=1"))
        goto ok;

    // qeury ap
    if (!strcmp(command, "AT+CWJAP?")) {
        if (WiFi.isConnected()) {
            Serial.printf("+CWJAP:\"%s\"\r\n", creds.ssid);
            goto ok;
        } else {
            Serial.print(F("No AP\r\n"));
            goto error;
        }
    }

    // connect to AP
    if (len > 9 && (!strncmp(command, "AT+CWJAP=", 9))) {
        char *src = command + 9;
        char *dst;
        char ssid[33];
        char pass[64];

        // check start double quote
        if (*src != '"')
            goto error;

        // parse ssid
        src++;
        dst = ssid;
        while (*src != '"' && *src != '\0' && dst - ssid < (int)sizeof(ssid) - 1) {
            if (*src == '\\')
                src++;
            *dst++ = *src++;
        }

        // check delimiter
        if (*src++ != '"' || *src++ != ',' || *src++ != '"')
            goto error;

        *dst = '\0';

        // hide password in history
        strcpy(history->buffer + (src - command), "*\"");

        // parse password
        dst = pass;
        while (*src != '"' && *src != '\0' && dst - pass < (int)sizeof(pass) - 1) {
            if (*src == '\\')
                src++;
            *dst++ = *src++;
        }

        // check end double quote
        if (*src != '"')
            goto error;

        *dst = '\0';

        // check if ssid and pass was changed
        if (strcmp(ssid, creds.ssid) || strcmp(pass, creds.pass)) {
            // copy new creds
            memset(creds.ssid, 0, sizeof(creds.ssid));
            memset(creds.pass, 0, sizeof(creds.pass));
            strcpy(creds.ssid, ssid);
            strcpy(creds.pass, pass);
            creds.crc = creds_crc(&creds);

            // save creds to eeprom
            EEPROM.put(0, creds);
            EEPROM.commit();

            // connect to wifi
            WiFi.disconnect();
            WiFi.begin(creds.ssid, creds.pass);
        }

        if (WiFi.waitForConnectResult(15000) != WL_CONNECTED) {
            Serial.print(F("+CWJAP:1\r\n\r\nFAIL\r\n"));
            return;
        }

        goto ok;
    }

    // set ip
    //  * WiFi connects only to DHCP-enabled networks
    //  * there is no any sense to set static ip
    if (len > 10 && (!strncmp(command, "AT+CIPSTA=", 10)))
        goto ok;

    // start/stop server
    if (len > 13 && (!strncmp(command, "AT+CIPSERVER=", 13))) {
        int cmd, port, r;

        r = sscanf(command + 13, "%d,%d", &cmd, &port);

        if (r > 0 && cmd == 0) {
            stop_server();
            goto ok;
        }

        if (r == 2 && cmd == 1 && port > 0 && port < 65536 && port != 8080 && server == nullptr) {
            server = new WiFiServer(port);
            rtc_usermem_set(port);
            server->begin();
            goto ok;
        }

        goto error;
    }

    // close client connection
    if (len > 12 && (!strncmp(command, "AT+CIPCLOSE=", 12))) {
        int n;

        if (sscanf(command + 12, "%d", &n) != 1)
            goto error;

        if (n < 0 || n >= MAX_CLIENTS)
            goto error;
        
        if (client[n] == nullptr) {
            Serial.print(F("link is not\r\n"));
            goto error;
        }

        client[n]->stop();
        delete client[n];
        client[n] = nullptr;
        Serial.printf("%d,CLOSED\r\n", n);

        goto ok;
    }

    // send data
    if (len > 11 && (!strncmp(command, "AT+CIPSEND=", 11))) {
        int i, l;

        if (sscanf(command + 11, "%d,%d", &i, &l) != 2)
            goto error;

        if (i < 0 || i >= MAX_CLIENTS)
            goto error;

        if (client[i] == nullptr || !client[i]->connected()) {
            Serial.print(F("link is not\r\n"));
            return;
        }

        if (len > (int)sizeof(send_buffer)) {
            Serial.print(F("tool long\r\n"));
            return;
        }

        send_to = i;
        send_pos = 0;
        send_len = l;
        Serial.print(F("> "));
        return;
    }

error:
    Serial.print(F("\r\nERROR\r\n"));
    return;
ok:
    Serial.print(F("\r\nOK\r\n"));
}


// handle / page
void handle_root()
{
    char buf[24];
    int i;

    strcpy_P(buffer, PSTR("MSR23 WiFi ModBus modem\n\nAT history:\n"));

    for (i = 0; i < HISTSIZE; i++) {
        history = history->next;
        strcat(buffer, "> ");
        strcat(buffer, history->buffer);
        strcat(buffer, "\n");
    }

    strcat_P(buffer, PSTR("\nRSSI: "));
    snprintf(buf, sizeof(buf), "%d", WiFi.RSSI());
    strcat(buffer, buf);

    strcat_P(buffer, PSTR(", uptime: "));
    snprintf(buf, sizeof(buf), "%llu", millis64() / 1000);
    strcat(buffer, buf);

    strcat_P(buffer, PSTR(" sec, reset reason: "));
    strcat(buffer, ESP.getResetReason().c_str());

    httpServer.send(200, "text/plain", buffer, strlen(buffer));
}


// setup
void setup()
{
    unsigned i;
    int port;

    // init history
    for (i = 0; i < HISTSIZE; i++) {
        h[i].buffer[0] = '\0';
        if (i > 0)
            h[i].next = &h[i - 1];
    }

    h[0].next = &h[HISTSIZE - 1];
    history = &h[0];

    // init wifi station mode
    WiFi.mode(WIFI_STA);

    // init creds and connect to wifi
    EEPROM.begin(512);
    memset(&creds, 0, sizeof(creds));
    EEPROM.get(0, creds);
    if (creds.crc == creds_crc(&creds)) {
        WiFi.begin(creds.ssid, creds.pass);
    } else {
        creds.ssid[0] = '\0';
        creds.pass[0] = '\0';
    }

    // init http server
    httpUpdater.setup(&httpServer, "/firmware", "admin", fwpw);
    httpServer.on("/", handle_root);
    httpServer.begin();

    // init server
    if (rtc_usermem_get(&port) && port > 0) {
        // spurious reset?
        server = new WiFiServer(port);
        server->begin();
    }

    // init serial
    Serial.begin(115200);
    Serial.println(F("\r\nready"));
}


// loop
void loop()
{
    WiFiClient newClient;

    // check serial input
    if (Serial.available() > 0) {
        // read byte
        int c = Serial.read();

        if (c >= 0) {
            // echo
            Serial.write(c);

            if (send_len > 0) {
                // at+cipsend
                send_buffer[send_pos++] = c;
                send_len--;

                if (send_len == 0) {
                    client[send_to]->write((uint8_t *)send_buffer, send_pos);
                    Serial.print(F("\r\nSEND OK\r\n"));
                    send_pos = 0;
                    send_to = -1;
                }
            } else {
                // at command
                input_buffer[input_pos++] = c;

                // check EOL
                if (c == '\n') {
                    size_t size = input_pos - 1;

                    if (size > 0 && input_buffer[size - 1] == '\r')
                        size--;

                    input_buffer[size] = '\0';
                    process_command(input_buffer, size);
                    input_pos = 0;
                }

                // check buffer overflow
                if (input_pos >= (int)sizeof(input_buffer))
                    input_pos = 0;
            }
        }
    }

    // handle stat server requests
    httpServer.handleClient();

    // check new connection
    if (server != nullptr) {
        newClient = server->available();

        if (newClient) {
            // search first free client slot
            for (int i = 0 ; i < MAX_CLIENTS; i++) {
                if (nullptr == client[i]) {
                    client[i] = new WiFiClient(newClient);
                    Serial.printf("%d,CONNECT\r\n", i);
                    break;
                }
            }
        }
    }

    // check connected clients
    for (int i = 0 ; i < MAX_CLIENTS; i++) {
        int available;
        int len;

        // empty slot
        if (client[i] == nullptr)
            continue;

        // client disconnected
        if (!client[i]->connected()) {
            client[i]->stop();
            delete client[i];
            client[i] = nullptr;
            Serial.printf("%d,CLOSED\r\n", i);
            if (send_len > 0 && send_to == i) {
                send_pos = 0;
                send_len = 0;
                send_to = -1;
            }
            continue;
        }

        // get available bytes
        available = client[i]->available();

        if (available <= 0)
            continue;

        if (available > (int)sizeof(buffer))
            available = sizeof(buffer);

        len = client[i]->read((uint8_t *)buffer, available);
        Serial.printf("+IPD,%d,%d:", i, len);
        Serial.write(buffer, len);
        Serial.print(F("\r\nOK\r\n"));
    }

    // update high32
    (void)millis64();
}