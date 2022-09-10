#include <sqlite3.h>
#include <LittleFS.h>
#include <ESP32QRCodeReader.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <ESP32Time.h>
#include <RTClib.h>
#include <Wire.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <DFRobotDFPlayerMini.h>
#include <Adafruit_SSD1306.h>
#include <Regexp.h>
#include <lvgl.h>
#include <Update.h>

#define FORMAT_LITTLEFS_IF_FAILED true
#define R 6371000.0

float firmware_version = 0.3;

char *zErrMsg;
char *name = NULL;
char *wifi_ssid = NULL;
char *wifi_pass = NULL;
char *ntp_server = NULL;
char *ip_label_text = NULL;

IPAddress ip;
IPAddress net_mask;
IPAddress gw;
IPAddress dns;

double lat = -100, lon = -200;
int rad = -1;
int time_window = -1;
int time_offset = 0;
int volume = 30;
int rly_on_time = 1000;
uint64_t main_timer = 0;
uint64_t wifi_timer;
uint64_t time_timer = 0;
uint64_t update_timer = 0;
int relay_pin = 2;

bool wifi_connecting = false;
bool webserver_started = false;
bool wifi_available = false;
bool ds_available = false;
bool ntp_enabled = false;
bool display_available = false;
bool player_available = false;
bool dhcp = true;
bool update_rtc_via_ntp = false;
bool relay_state = false;
bool update_ip = true;
bool update_prepared = false;
bool ip_label_changed = false;
bool rly_ctl_enabled = false;

static const uint16_t screenWidth  = 128;
static const uint16_t screenHeight = 64;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[ screenWidth * 10 ];

sqlite3 *db;
ESP32QRCodeReader reader(CAMERA_MODEL_AI_THINKER, FRAMESIZE_HQVGA);
WebServer server(80);
ESP32Time rtc;
RTC_DS3231 ds;
WiFiUDP ntp_udp;
NTPClient *ntp_client;
DFRobotDFPlayerMini player;
Adafruit_SSD1306 display(128, 64, &Wire, -1, 800000UL, 400000UL);
WiFiClient stream;

lv_obj_t *time_label;
lv_obj_t *ip_label;
lv_obj_t *status_label;
lv_obj_t *result_label;

TaskHandle_t qr_task;

#if LV_USE_LOG != 0
void my_print(const char * buf){
    Serial.printf(buf);
    Serial.flush();
}
#endif

void my_disp_flush( lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p ){
    uint32_t w = ( area->x2 - area->x1 + 1 );
    uint32_t h = ( area->y2 - area->y1 + 1 );
    uint8_t* buf = (uint8_t*) color_p;
    for (size_t y = 0; y < h; y++){
        for (size_t x = 0; x < w; x++){
            display.drawPixel(area->x1 + x, area->y1 + y, *buf);
            buf++;
        }
    }
    display.display();

    lv_disp_flush_ready( disp );
}

struct QRCodeData qr_code_data;

void print_reset_reason(){
    int reason = esp_reset_reason();
    switch (reason){
        case 1  : Serial.println ("Vbat power on reset");break;
        case 3  : Serial.println ("Software reset digital core");break;
        case 4  : Serial.println ("Legacy watch dog reset digital core");break;
        case 5  : Serial.println ("Deep Sleep reset digital core");break;
        case 6  : Serial.println ("Reset by SLC module, reset digital core");break;
        case 7  : Serial.println ("Timer Group0 Watch dog reset digital core");break;
        case 8  : Serial.println ("Timer Group1 Watch dog reset digital core");break;
        case 9  : Serial.println ("RTC Watch dog Reset digital core");break;
        case 10 : Serial.println ("Instrusion tested to reset CPU");break;
        case 11 : Serial.println ("Time Group reset CPU");break;
        case 12 : Serial.println ("Software reset CPU");break;
        case 13 : Serial.println ("RTC Watch dog Reset CPU");break;
        case 14 : Serial.println ("for APP CPU, reseted by PRO CPU");break;
        case 15 : Serial.println ("Reset when the vdd voltage is not stable");break;
        case 16 : Serial.println ("RTC Watch dog reset digital core and rtc module");break;
        default : Serial.println ("NO_MEAN");
    }
}

static int select_users_callback(void *data, int argc, char **argv, char **col_name){
    char *buffer = (char*)malloc(75);
    sprintf(buffer, "{\"id\":%s,\"name\":\"%s\"},", argv[0], argv[1]);
    strcat((char*)data, buffer);
    free(buffer);
    return 0;
}

static int select_events_callback(void *data, int argc, char **argv, char **col_name){
    char *buffer = (char*)malloc(45);
    sprintf(buffer, "{\"ts\":%s,\"id\":%s,\"status\":%s},", argv[0], argv[1], argv[2]);
    strcat((char*)data, buffer);
    free(buffer);
    return 0;
}

static int count_users_callback(void *data, int argc, char **argv, char **col_name){
    sprintf((char*)data, "%s", argv[0]);
    return 0;
}

static int get_user_name_callback(void *data, int argc, char **argv, char **col_name){
    sprintf((char*)data, "%s", argv[0]);
    return 0;
}

uint32_t _now(){
    if (ntp_enabled){
        return ntp_client->getEpochTime();
    }
    if (ds_available){
        return ds.now().unixtime();
    }
    return rtc.getEpoch();
}

void init_display(){
    Serial.print(F("Initializing display..."));
    lv_init();
    if(display.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false)) {
        Serial.print(F("Done\r\n"));

        display.clearDisplay();
        display.display();

        lv_disp_draw_buf_init( &draw_buf, buf, NULL, screenWidth * 10 );
#if LV_USE_LOG != 0
        lv_log_register_print_cb(my_print); /* register print function for debugging */
#endif
        /*Initialize the display*/
        static lv_disp_drv_t disp_drv;
        lv_disp_drv_init( &disp_drv );
        /*Change the following line to your display resolution*/
        disp_drv.hor_res = screenWidth;
        disp_drv.ver_res = screenHeight;
        disp_drv.flush_cb = my_disp_flush;
        disp_drv.draw_buf = &draw_buf;
        lv_disp_drv_register( &disp_drv );

        time_label = lv_label_create(lv_scr_act());
        lv_label_set_text(time_label, "");
        lv_obj_set_style_text_font(time_label, &lv_font_unscii_8, 0);

        lv_obj_t *label = lv_label_create(lv_scr_act());
        lv_obj_set_pos(label, 80, 0);
        char *buffer = (char*)malloc(10);
        lv_obj_set_style_text_font(label, &lv_font_unscii_8, 0);
        sprintf(buffer, "fw:%3.1f", firmware_version);
        lv_label_set_text(label, buffer);
        free(buffer);

        ip_label = lv_label_create(lv_scr_act());
        lv_obj_set_pos(ip_label, 5, 14);
        lv_obj_set_style_text_font(ip_label, &lv_font_montserrat_14, 0);
        ip_label_text = (char*)malloc(25);
        lv_label_set_text(ip_label, "IP:Not connected");

        status_label = lv_label_create(lv_scr_act());
        lv_obj_set_pos(status_label, 3, 53);
        lv_obj_set_style_text_font(status_label, &lv_font_montserrat_10, 0);
        lv_label_set_text(status_label, "Initializing");

        result_label = lv_label_create(lv_scr_act());
        lv_obj_set_pos(result_label, 0, 29);
        lv_obj_set_size(result_label, 128, 25);
        lv_obj_set_style_text_font(result_label, &lv_font_dejavu_16_persian_hebrew, 0);
        lv_obj_set_style_text_align(result_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_base_dir(result_label, LV_BASE_DIR_RTL, 0);
        lv_label_set_long_mode(result_label, LV_LABEL_LONG_SCROLL);
        lv_label_set_text(result_label, "");
        lv_timer_handler();
        display_available = true;
    }else{
        Serial.print(F("Failed\r\n"));
    }
}

void init_time(){
    Serial.printf("Checking DS3231...");
    for (int i = 0; i < 3; i++){
        if (ds.begin(&Wire)){
            ds_available = true;
            Serial.printf("Found.\r\n");
            break;
        }
        delay(10);
    }
    if (!ds_available){
        Serial.printf("Not found!\r\n");
    }

    if (ntp_enabled && wifi_available){
        ntp_client->begin();
        Serial.printf("Initialized NTP client %s\r\n", ntp_server);
    }
}

bool init_fs(){
    Serial.print(F("Mounting file system..."));
    if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED, "/fs")){
        Serial.print(F("Failed\r\n"));
        return false;
    }
    Serial.print(F("Done\r\n"));
    return true;
}

int init_db(){
    Serial.print(F("Opening database..."));
    int rc = sqlite3_open("/fs/data.db", &db);
    if (rc){
        Serial.printf("Failed: %s\r\n", sqlite3_errstr(rc));
        return -1;
    }
    Serial.print(F("Done\r\n"));
    sqlite3_extended_result_codes(db, 1);
    int res;
    Serial.printf("Enabling Foreign key constraint...");
    rc = sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_FKEY, 1, &res);
    if (rc == SQLITE_OK && res == 1){
        Serial.print(F("Done\r\n"));
    }
    else{
        Serial.print(F("Failed\r\n"));
    }

    Serial.print(F("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT NOT NULL);..."));
    rc = sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT NOT NULL);", NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        sqlite3_free(zErrMsg);
        return -2;
    }
    Serial.print(F("Done\r\n"));

    Serial.print(F("CREATE TABLE IF NOT EXISTS events (ts INTEGER, id INTEGER, status INTEGER, PRIMARY KEY(ts, id), FOREIGN KEY (id) REFERENCES users (id) ON UPDATE CASCADE ON DELETE CASCADE);"));
    rc = sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS events (ts INTEGER, id INTEGER, status INTEGER, PRIMARY KEY(ts, id), FOREIGN KEY (id) REFERENCES users (id) ON UPDATE CASCADE ON DELETE CASCADE);",NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        sqlite3_free(zErrMsg);
        return -2;
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

int insert_users(JsonArray &users){
    char *query = (char*)malloc(30 + users.size() * 65);
    sprintf(query, "INSERT INTO users VALUES");
    for (JsonVariant user: users){
        char *buffer = (char*)malloc(60);
        sprintf(buffer, "(%d,'%s'),", user["id"].as<int>(), user["name"].as<const char*>()); 
        strcat(query, buffer);
    }
    query[strlen(query) - 1] = ';';
    Serial.printf("%s...", query);
    int rc = sqlite3_exec(db, query, NULL, NULL, &zErrMsg);
    free(query);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        return rc;
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

int select_users(char *data, bool condition, char *filter){
    char *query;
    if (condition){
        query = (char*)malloc(30 + strlen(filter));
        sprintf(query, "SELECT * FROM users WHERE %s;", filter);
    }else{
        query = (char*)malloc(25);
        sprintf(query, "SELECT * FROM users;");
    }
    Serial.printf("%s...", query);
    sprintf(data, "[");
    int rc = sqlite3_exec(db, query, select_users_callback, (void*)data, &zErrMsg);
    free(query);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        return rc;
    }
    if (strlen(data) == 1){
        sprintf(data, "[]");
    }else{
        data[strlen(data) - 1] = ']';
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

int get_user_name(int id, char *name){
    char *query = (char*)malloc(40);
    sprintf(query, "SELECT name FROM users WHERE id=%d;", id);
    Serial.printf("%s...", query);
    int rc = sqlite3_exec(db, query, get_user_name_callback, (void*)name, &zErrMsg);
    free(query);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        return rc;
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

int delete_users(JsonArray &ids){
    char *query = (char*)malloc(35 + ids.size() * 5);
    sprintf(query, "DELETE from users WHERE id IN (");
    for (JsonVariant id: ids){
        sprintf(query, "%s%d,", query, id.as<int>()); 
    }
    int len = strlen(query);
    query[len - 1] = ')';
    query[len] = ';';
    query[len + 1] = 0;
    Serial.printf("%s...", query);
    int rc = sqlite3_exec(db, query, NULL, NULL, &zErrMsg);
    free(query);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        return rc;
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

int delete_all_users(){
    char *query = (char*)malloc(25);
    sprintf(query, "DELETE from users;");
    Serial.printf("%s...", query);
    int rc = sqlite3_exec(db, query, NULL, NULL, &zErrMsg);
    free(query);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        return rc;
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

int delete_all_events(){
    char *query = (char*)malloc(25);
    sprintf(query, "DELETE from events;");
    Serial.printf("%s...", query);
    int rc = sqlite3_exec(db, query, NULL, NULL, &zErrMsg);
    free(query);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        return rc;
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

int insert_event(uint32_t ts, uint16_t id, uint8_t status){
    char *query = (char*)malloc(55);
    sprintf(query, "INSERT INTO events VALUES (%u,%d,%d);", ts, id, status);
    Serial.printf("%s...", query);
    int rc = sqlite3_exec(db, query, NULL, NULL, &zErrMsg);
    free(query);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        return rc;
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

int select_events(char *data, const char* from, const char* to, int offset){
    char *query = (char*)malloc(100);
    if (from && to){
        sprintf(query, "SELECT * FROM events WHERE ts BETWEEN %s AND %s LIMIT 1000 OFFSET %d;", from, to, offset);
    }
    else if (from) {
        sprintf(query, "SELECT * FROM events WHERE ts >= %s LIMIT 1000 OFFSET %d;", from, offset);
    }
    else if (to) {
        sprintf(query, "SELECT * FROM events WHERE ts <= %s LIMIT 1000 OFFSET %d;", to, offset);
    }
    else{
        sprintf(query, "SELECT * FROM events LIMIT 1000 OFFSET %d;", offset);
    }



    Serial.printf("%s...", query);
    sprintf(data, "[");
    int rc = sqlite3_exec(db, query, select_events_callback, (void*)data, &zErrMsg);
    free(query);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        return rc;
    }
    if (strlen(data) == 1){
        sprintf(data, "[]");
    }else{
        data[strlen(data) - 1] = ']';
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

int count_users(char *data){
    char *query = (char*)malloc(35);
    sprintf(query, "SELECT COUNT(_rowid_) FROM users;");
    Serial.printf("%s...", query);
    int rc = sqlite3_exec(db, query, count_users_callback, (void*)data, &zErrMsg);
    free(query);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        return rc;
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

int count_events(char *data){
    char *query = (char*)malloc(35);
    sprintf(query, "SELECT COUNT(_rowid_) FROM events;");
    Serial.printf("%s...", query);
    int rc = sqlite3_exec(db, query, count_users_callback, (void*)data, &zErrMsg);
    free(query);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        return rc;
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

void wifi_event_handler(WiFiEvent_t event, WiFiEventInfo_t info){
    Serial.printf("WiFi event: %d\r\n", event);
}

void init_wifi(){
    Serial.print(F("Initializing WiFi..."));
    if (!wifi_ssid){
        Serial.printf("SSID not set\r\n");
        wifi_available = false;
        return;
    }
    Serial.print(F("Done\r\n"));
    wifi_available = true;
    WiFi.mode(WIFI_STA);

    if (!dhcp){
        Serial.print(F("Setting static IP\r\n"));
        WiFi.config(ip, gw, net_mask, dns);
    }

    if (!wifi_pass){
        Serial.printf("Connecting to %s with no password\r\n", wifi_ssid);
        WiFi.begin(wifi_ssid);
    }else{
        Serial.printf("Connecting to %s with password %s\r\n", wifi_ssid, wifi_pass); // TODO: remove password printing
        WiFi.begin(wifi_ssid, wifi_pass);
    }
    wifi_connecting = true;
    wifi_timer = millis();
    // WiFi.onEvent(wifi_event_handler);
}

void check_wifi(){
    // Serial.print(F("Checking WiFi..."));
    if (!wifi_available){
        update_ip = false;
        // Serial.print(F("not available!\r\n"));
        return;
    }

    if (WiFi.isConnected()){
        // Serial.print(F("connected\r\n"));
        if (update_ip){
            update_ip = false;
            sprintf(ip_label_text, "IP: %s", WiFi.localIP().toString());
            ip_label_changed = true;
        }
        wifi_connecting = false;
        if (!webserver_started){
            init_webserver();
            Serial.printf("Web server started %s\r\n", WiFi.localIP().toString());
        }
        // if (!stream.connected()){
        //     if(stream.connect("192.168.1.13", 8090)){
        //         Serial.println("Stream connected");
        //     }
        // }
        return;
    }

    if (wifi_connecting){
        if (millis() - wifi_timer > 10000){
            wifi_connecting = false;
            Serial.print(F("connection timeout\r\n"));
            sprintf(ip_label_text, "IP:Timedout");
            ip_label_changed = true;
        }else{
            Serial.print(F("connecting\r\n"));
            sprintf(ip_label_text, "IP:Connecting");
            ip_label_changed = true;
            return;
        }
    }
    update_ip = true;
    WiFi.reconnect();
    wifi_connecting = true;
    wifi_timer = millis();
    Serial.print(F("reconnecting\r\n"));
    sprintf(ip_label_text, "IP:Connecting");
    ip_label_changed = true;
    return;
}

bool get_request_data(JsonDocument &data){
    if(server.hasArg("plain")){
        DeserializationError error = deserializeJson(data, server.arg("plain"));
        if (error){
            return false;
        }
        return true;
    }
    for (size_t i = 0; i < server.args(); i++){
        data[server.argName(i)] = server.arg(i);
    }
    return true;
}

bool read_config(JsonDocument &config){
    File file = LittleFS.open("/config.json");
    if (!file){
        return false;
    }
    deserializeJson(config, file);
    file.close();
    return true;
}

bool write_config(JsonDocument &config){
    File file = LittleFS.open("/config.json", "w");
    if (!file){
        return false;
    }
    serializeJson(config, file);
    file.close();
    return true;
}

void get_config(){
    Serial.print(F("GET /get_config\r\n"));
    DynamicJsonDocument config(512);
    stop_scanner();
    if (!read_config(config)){
        start_scanner();
        server.send(200, "application/json", "{}");
        return;
    }
    start_scanner();
    String body;
    serializeJsonPretty(config, body);
    Serial.printf("Response: %s\r\n", body.c_str());
    server.send(200, "application/json", body);
}

template <typename T>
void update_value(JsonDocument &config, 
                  JsonDocument &data,
                  JsonDocument &keys,
                  const char *key){
    if (data.containsKey(key)){
        config[key] = data[key].as<T>();
        data.remove(key);
        keys.add(key);
    }
}

void set_config(){
    Serial.print(F("POST /set_config\r\n"));
    DynamicJsonDocument data(512);
    String body;
    if(!get_request_data(data)){
        data.clear();
        data["result"] = false;
        data["msg"] = "Invalid request";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    serializeJsonPretty(data, Serial);
    Serial.print("\r\n");

    DynamicJsonDocument config(512);
    DynamicJsonDocument keys(512);
    stop_scanner();
    read_config(config);

    update_value<const char*>(config, data, keys, "name");
    update_value<const char*>(config, data, keys, "ssid");
    update_value<const char*>(config, data, keys, "pass");
    update_value<double>(config, data, keys, "lat");
    update_value<double>(config, data, keys, "lon");
    update_value<int>(config, data, keys, "rad");
    update_value<int>(config, data, keys, "time_window");
    update_value<bool>(config, data, keys, "ntp");
    update_value<const char*>(config, data, keys, "ntp_server");
    update_value<int>(config, data, keys, "time_offset");
    update_value<bool>(config, data, keys, "dhcp");
    update_value<const char*>(config, data, keys, "ip");
    update_value<const char*>(config, data, keys, "mask");
    update_value<const char*>(config, data, keys, "gw");
    update_value<const char*>(config, data, keys, "dns");
    update_value<int>(config, data, keys, "volume");
    update_value<bool>(config, data, keys, "relay");
    update_value<int>(config, data, keys, "relay_on_time");

    if (!write_config(config)){
        start_scanner();
        data.clear();
        data["result"] = false;
        data["msg"] = "Failed to save config file";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    start_scanner();
    data.clear();
    data["result"] = true;
    data["msg"] = "config saved successfully";
    data["processed_keys"] = keys;
    serializeJsonPretty(data, body);
    Serial.printf("Response: %s\r\n", body.c_str());
    server.send(200, "application/json", body);
}

void add_user(){
    Serial.print(F("POST /add_user\r\n"));
    DynamicJsonDocument data(128);
    String body;

    char *buffer = (char*)malloc(10);
    stop_scanner();
    int rc = count_users(buffer);
    if (rc != SQLITE_OK){
        start_scanner();
        free(buffer);
        data["result"] = false;
        data["msg"] = "Database error";
        serializeJsonPretty(data, body);
        Serial.printf("Response 1: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }

    long cnt = 0;
    char *ptr;
    cnt = strtol(buffer, &ptr, 10);
    free(buffer);
    if (cnt >= 100){
        start_scanner();
        data["result"] = false;
        data["msg"] = "Maximum user count reached(100)";
        serializeJsonPretty(data, body);
        Serial.printf("Response 2: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }

    if(!get_request_data(data)){
        start_scanner();
        data.clear();
        data["result"] = false;
        data["msg"] = "Invalid request";
        serializeJsonPretty(data, body);
        Serial.printf("Response 3: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    serializeJsonPretty(data, Serial);
    Serial.print("\r\n");
    if (!data.containsKey("id") || !data.containsKey("name")){
        start_scanner();
        data.clear();
        data["result"] = false;
        data["msg"] = "User id  or name not specified";
        serializeJsonPretty(data, body);
        Serial.printf("Response 4: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    int id = data["id"].as<int>();
    if (id <= 0 || id > 255){
        start_scanner();
        data.clear();
        data["result"] = false;
        data["msg"] = "User id not valid, id must be integer and in range [1, 255]";
        serializeJsonPretty(data, body);
        Serial.printf("Response 5: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }

    if (!data["name"].as<const char*>()){
        start_scanner();
        data.clear();
        data["result"] = false;
        data["msg"] = "User name not valid";
        serializeJsonPretty(data, body);
        Serial.printf("Response 6: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }

    if (strlen(data["name"].as<const char*>()) > 50){
        start_scanner();
        data.clear();
        data["result"] = false;
        data["msg"] = "User name can't be more than 50 charachters";
        serializeJsonPretty(data, body);
        Serial.printf("Response 7: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    StaticJsonDocument<128> doc;
    JsonArray users = doc.to<JsonArray>();
    users.add(data);
    serializeJson(users, Serial);
    Serial.println();
    rc = insert_users(users);
    start_scanner();
    if (rc != 0){
        data.clear();
        data["result"] = false;
        if(rc == SQLITE_CONSTRAINT_UNIQUE){
            data["msg"] = "User exists";
        }
        else{
            data["msg"] = "Failed to add user";
        }
        serializeJsonPretty(data, body);
        Serial.printf("Response 8: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    data.clear();
    data["result"] = true;
    data["msg"] = "User added successfully";
    serializeJsonPretty(data, body);
    Serial.printf("Response: %s\r\n", body.c_str());
    server.send(200, "application/json", body);
}

void add_users(){
    Serial.print(F("POST /add_users\r\n"));
    DynamicJsonDocument data(12288);
    String body;

    char *buffer = (char*)malloc(10);
    stop_scanner();
    int rc = count_users(buffer);
    if (rc != SQLITE_OK){
        start_scanner();
        free(buffer);
        data["result"] = false;
        data["msg"] = "Database error";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }

    long cnt = 0;
    char *ptr;
    cnt = strtol(buffer, &ptr, 10);
    free(buffer);
    if (cnt >= 100){
        start_scanner();
        data["result"] = false;
        data["msg"] = "Maximum user count reached(100)";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }

    if(!get_request_data(data)){
        start_scanner();
        data["result"] = false;
        data["msg"] = "Invalid request";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    serializeJsonPretty(data, Serial);
    Serial.print("\r\n");

    JsonArray users = data.as<JsonArray>();
    bool users_valid = true;
    JsonVariant invalid_user;
    for (JsonVariant user: users){
        if (!user["id"].as<int>() || user["id"].as<int>() > 255 || strlen(user["name"].as<const char *>()) > 50 || strlen(user["name"].as<const char *>()) == 0){
            users_valid = false;
            invalid_user = user;
            break;
        }
    }

    if (!users || users.size() == 0){
        start_scanner();
        data.clear();
        data["result"] = false;
        data["msg"] = "User list not valid";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }

    if (!users_valid){
        start_scanner();
        data.clear();
        data["result"] = false;
        char *buffer = (char*)malloc(100);
        sprintf(buffer, "invalid user: {\"id\":%d,\"name\":\"%s\"}", invalid_user["id"].as<int>(), invalid_user["name"].as<const char*>());
        data["msg"] = String(buffer);
        free(buffer);
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }

    rc = insert_users(users);
    start_scanner();
    if (rc != 0){
        data.clear();
        data["result"] = false;
        if(rc == SQLITE_CONSTRAINT_UNIQUE){
            data["msg"] = "one or more of the Users exists, no users added";
        }
        else{
            data["msg"] = "Failed to add user";
        }
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    data.clear();
    data["result"] = true;
    data["msg"] = "Users added successfully";
    serializeJsonPretty(data, body);
    Serial.printf("Response: %s\r\n", body.c_str());
    server.send(200, "application/json", body);
}

void get_users(){
    Serial.print(F("GET /get_users\r\n"));
    char *data = (char*)malloc(7500);
    stop_scanner();
    select_users(data, false, (char*)"");
    start_scanner();
    Serial.printf("Response: %s\r\n", data);
    server.send_P(200, "application/json", data);
    free(data);
}

void del_user(){
    Serial.print(F("POST /delete_user\r\n"));
    DynamicJsonDocument data(32);
    String body;
    if(!get_request_data(data)){
        data.clear();
        data["result"] = false;
        data["msg"] = "Invalid request";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    serializeJsonPretty(data, Serial);
    Serial.print("\r\n");
    if (!data.containsKey("id")){
        data.clear();
        data["result"] = false;
        data["msg"] = "User id not specified";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    int id = data["id"].as<int>();
    if (id <= 0){
        data.clear();
        data["result"] = false;
        data["msg"] = "User id not valid, id must be integer and >= 1";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    char *result = (char*)malloc(75);
    char *filter = (char*)malloc(10);
    sprintf(filter, "id=%d", id);
    stop_scanner();
    select_users(result, true, filter);
    deserializeJson(data, result);
    free(result);
    free(filter);
    if (data.size() == 0){
        start_scanner();
        data.clear();
        data["result"] = false;
        data["msg"] = "User does not exist";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    StaticJsonDocument<JSON_ARRAY_SIZE(1)> doc;
    JsonArray ids = doc.to<JsonArray>();
    ids.add(id);

    int rc = delete_users(ids);
    start_scanner();
    if (rc != 0){
        data.clear();
        data["result"] = false;
        data["msg"] = "Failed to delete user";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    data.clear();
    data["result"] = true;
    data["msg"] = "User deleted successfully";
    serializeJsonPretty(data, body);
    Serial.printf("Response: %s\r\n", body.c_str());
    server.send(200, "application/json", body);
}

void del_users(){
    Serial.print(F("POST /delete_users\r\n"));
    DynamicJsonDocument data(2048);
    String body;
    if(!get_request_data(data)){
        data["result"] = false;
        data["msg"] = "Invalid request";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    serializeJsonPretty(data, Serial);
    Serial.print("\r\n");
    if (!data.containsKey("ids")){
        data.clear();
        data["result"] = false;
        data["msg"] = "User id list not specified";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    deserializeJson(data, data["ids"].as<const char*>());
    JsonArray ids = data.as<JsonArray>();
    bool ids_valid = true;
    for (JsonVariant id: ids){
        if (!id.as<int>()){
            ids_valid = false;
            break;
        }
    }
    
    if (!ids || !ids_valid || ids.size() == 0){
        data.clear();
        data["result"] = false;
        data["msg"] = "User id list not valid, id list must array of integer ids";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }

    int id_cnt = ids.size();
    char *result = (char*)malloc( 5 + 60 * id_cnt);
    char *filter = (char*)malloc(10 + 4 * id_cnt);
    serializeJson(ids, result, 5 + 4 * id_cnt);
    result[0] = '(';
    result[strlen(result) - 1] = ')';
    sprintf(filter, "id IN %s", result);
    stop_scanner();
    select_users(result, true, filter);
    DynamicJsonDocument temp(16384);
    deserializeJson(temp, result);
    free(result);
    free(filter);
    if (temp.size() != ids.size()){
        start_scanner();
        temp.clear();
        data.clear();
        data["result"] = false;
        data["msg"] = "one or more of the Users exists, no users deleted";
        serializeJsonPretty(data, Serial);
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }

    int rc = delete_users(ids);
    start_scanner();
    if (rc != 0){
        data.clear();
        data["result"] = false;
        data["msg"] = "Failed to delete users";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }

    data.clear();
    data["result"] = true;
    data["msg"] = "Users deleted successfully";
    serializeJsonPretty(data, body);
    Serial.printf("Response: %s\r\n", body.c_str());
    server.send(200, "application/json", body);
}

void get_time(){
    char *data = (char*)malloc(35);
    sprintf(data, "{\"ts\":%lu,\"offset\":%d}", _now() - time_offset, time_offset);
    Serial.printf("Response: %s\r\n", data);
    server.send_P(200, "application/json", data);
    free(data);
}

void ntp_update(){
    if (ntp_enabled){
        if (WiFi.isConnected() && ntp_client->update()){
            if (!update_rtc_via_ntp){
                Serial.print(F("set ds and rtc with ntp\r\n"));
                if (ds_available){
                    ds.adjust(DateTime(ntp_client->getEpochTime()));
                }
                rtc.setTime(ntp_client->getEpochTime());
                update_rtc_via_ntp = true;
            }
        }
    }
}

void set_time(){
    Serial.print(F("POST /set_time\r\n"));
    DynamicJsonDocument data(32);
    String body;
    if(!get_request_data(data)){
        data["result"] = false;
        data["msg"] = "Invalid request";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }
    serializeJsonPretty(data, Serial);
    Serial.print("\r\n");

    if (ntp_enabled){
        data.clear();
        data["result"] = false;
        data["msg"] = "disable ntp to set time manually";
        serializeJsonPretty(data, body);
        Serial.printf("Response: %s\r\n", body.c_str());
        server.send(400, "application/json", body);
        return;
    }

    if (ds_available){
        ds.adjust(DateTime(data["ts"].as<uint32_t>() + time_offset));
    }
    rtc.setTime(data["ts"].as<uint32_t>());
    rtc.offset = time_offset;

    data.clear();
    data["result"] = true;
    data["msg"] = "time set successfully";
    serializeJsonPretty(data, body);
    Serial.printf("Response: %s\r\n", body.c_str());
    server.send(200, "application/json", body);
}

void get_user_cnt(){
    Serial.print(F("GET /get_user_count\r\n"));
    char* data  = (char*)malloc(10);
    stop_scanner();
    int rc = count_users(data);
    start_scanner();
    if (rc != SQLITE_OK){
        free(data);
        Serial.printf("Response: 400\r\n");
        server.send(400);
        return;
    }
    Serial.printf("Response: %s\r\n", data);
    server.send_P(200, "application/json", data);
    free(data);
}

void get_event_cnt(){
    Serial.print(F("GET /get_event_count\r\n"));
    char* data  = (char*)malloc(10);
    stop_scanner();
    int rc = count_events(data);
    start_scanner();
    if (rc != SQLITE_OK){
        free(data);
        Serial.printf("Response: 400\r\n");
        server.send(400);
        return;
    }
    Serial.printf("Response: %s\r\n", data);
    server.send_P(200, "application/json", data);
    free(data);
}

void del_all_users(){
    Serial.print(F("DELETE /delete_all_users\r\n"));
    stop_scanner();
    int rc = delete_all_users();
    start_scanner();
    if (rc != SQLITE_OK){
        Serial.printf("Response: 400\r\n");
        server.send(400);
        return;
    }
    Serial.printf("Response: 200\r\n");
    server.send(200);
}

void del_all_events(){
    Serial.print(F("DELETE /delete_all_events\r\n"));
    stop_scanner();
    int rc = delete_all_events();
    start_scanner();
    if (rc != SQLITE_OK){
        Serial.printf("Response: 400\r\n");
        server.send(400);
        return;
    }
    Serial.printf("Response: 200\r\n");
    server.send(200);
}

void get_events(){
    Serial.print(F("POST /get_events\r\n"));
    DynamicJsonDocument data(128);
    String body;
    get_request_data(data);
    serializeJsonPretty(data, Serial);
    Serial.print("\r\n");
    int offset = 0;
    if (data.containsKey("offset")){
        offset = data["offset"].as<int>();
    }
    char *response = (char*)malloc(40960);
    int rc;
    stop_scanner();
    if (data.containsKey("from") && data.containsKey("to")){
        rc = select_events(response, data["from"].as<const char*>(), data["to"].as<const char*>(), offset);
    }
    else if (data.containsKey("from")){
        rc = select_events(response, data["from"].as<const char*>(), NULL, offset);
    }
    else if (data.containsKey("to")){
        rc = select_events(response, NULL, data["to"].as<const char*>(), offset);
    }
    else{
        rc = select_events(response, NULL, NULL, offset);
    }
    start_scanner();
    if (rc != SQLITE_OK){
        Serial.printf("Response: Database Error\r\n");
        server.send(400, "text/plain", "Database Error");
        return;
    }

    Serial.printf("response size: %d\r\n", strlen(response));
    server.send_P(200, "application/json", response);
    free(response);
}

void restart(){
    Serial.print("GET /restart\r\n");
    server.send(200, "text/plain", "Done");
    delay(1000);
    esp_restart();
}

void prepare_for_update(){
    update_prepared = true;
    stop_scanner();
    if (qr_task != NULL){
      vTaskDelete(qr_task);
    }
}

void abort_update(){
    Update.abort();
    start_scanner();
    start_qr_loop();
    update_timer = 0;
    update_prepared = false;
}

void update(){
    Serial.print("POST /update\r\n");
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    delay(1000);
    ESP.restart();
}

void upload(){
    // Serial.print("POST(upload) /update\r\n");
    if (!update_prepared){
        prepare_for_update();
        Serial.printf("Headers:\r\n");
    }
    HTTPUpload upload = server.upload();
    update_timer = millis();
    if (upload.status == UPLOAD_FILE_START){
        Serial.printf("Update: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)){ // start with max available size
            Update.printError(Serial);
            abort_update();
        }
        lv_label_set_text(status_label, "Updateing...");
        lv_timer_handler();
    }
    else if (upload.status == UPLOAD_FILE_WRITE){
        /* flashing firmware to ESP*/
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize){
            Update.printError(Serial);
            abort_update();
        }else{
            char *buffer = (char*)malloc(20);
            sprintf(buffer, "Updateing: %d%%", Update.progress() * 100 / 1572864);
            Serial.println(buffer);
            lv_label_set_text(status_label, buffer);
            lv_timer_handler();
            free(buffer);
        }
    }
    else if (upload.status == UPLOAD_FILE_END){
        if (Update.end(true)){ // true to set the size to the current progress
            Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
            lv_label_set_text(status_label, "Updateing: 100%");
            lv_timer_handler();
        }
        else{
            Update.printError(Serial);
            lv_label_set_text(status_label, "Update failed");
            lv_timer_handler();
            abort_update();
        }
    }
}

void init_webserver(){
    server.on("/restart", restart);
    server.on("/get_config", get_config);
    server.on("/get_users", get_users);
    server.on("/get_time", get_time);
    server.on("/get_user_count", get_user_cnt);
    server.on("/get_event_count", get_event_cnt);
    server.on("/get_events", get_events);
    server.on("/set_config", HTTP_POST, set_config);
    server.on("/set_time", HTTP_POST, set_time);
    server.on("/add_user", HTTP_POST, add_user);
    server.on("/add_users", HTTP_POST, add_users);
    server.on("/delete_user", HTTP_DELETE, del_user);
    server.on("/delete_users", HTTP_DELETE, del_users);
    server.on("/delete_all_users", HTTP_DELETE, del_all_users);
    server.on("/delete_all_events", HTTP_DELETE, del_all_events);
    server.on("/update", HTTP_POST, update, upload);
    server.begin();
    webserver_started = true;
}

void load_config(){
    Serial.print(F("Loading config..."));
    DynamicJsonDocument config(512);
    if (!read_config(config)){
        Serial.print(F("config not available\r\n"));
        return;
    }
    Serial.print(F("Done\r\n"));
    if (config.containsKey("name")){
        name = (char*)malloc(strlen(config["name"].as<const char*>()) + 1);
        sprintf(name, "%s", config["name"].as<const char*>());
        Serial.printf("Device name: %s\r\n", name);
    }

    if (config.containsKey("ssid")){
        wifi_ssid = (char*)malloc(strlen(config["ssid"].as<const char*>()) + 1);
        sprintf(wifi_ssid, "%s", config["ssid"].as<const char*>());
        Serial.printf("WiFI SSID: %s\r\n", wifi_ssid);
    }

    if (config.containsKey("pass")){
        wifi_pass = (char*)malloc(strlen(config["pass"].as<const char*>()) + 1);
        sprintf(wifi_pass, "%s", config["pass"].as<const char*>());
        Serial.printf("WiFI KEY: %s\r\n", wifi_pass);
    }

    if (config.containsKey("lat") && 
        config.containsKey("lon") && 
        config.containsKey("rad")){

        lat = config["lat"].as<double>();
        lon = config["lon"].as<double>();
        rad = config["rad"].as<int>();
        Serial.printf("location range: (%f, %f, %d)\r\n", lat, lon, rad);
    }

    if (config.containsKey("time_window")){
        time_window = config["time_window"].as<int>();
        Serial.printf("Time window : %d \r\n", time_window);
    }

    if (config.containsKey("ntp")){
        if (config["ntp"].as<bool>()){
            if (config.containsKey("ntp_server") && strlen(config["ntp_server"].as<const char*>()) > 0){
                ntp_enabled = true;
                ntp_server = (char*)malloc(strlen(config["ntp_server"].as<const char*>()) + 1);
                sprintf(ntp_server, "%s", config["ntp_server"].as<const char*>());
                ntp_client = new NTPClient(ntp_udp, ntp_server);
                Serial.printf("NTP server: %s\r\n", ntp_server);
            }
        }
    }

    if (config.containsKey("time_offset")){
        time_offset = config["time_offset"].as<int>();
        if (ntp_enabled){
            ntp_client->setTimeOffset(time_offset);
        }
        rtc.offset = time_offset;
        Serial.printf("Time offset: %d\r\n", time_offset);
    }

    if (config.containsKey("dhcp")){
        if (!config["dhcp"].as<bool>()){
            if (config.containsKey("ip") && 
                config.containsKey("mask") && 
                config.containsKey("gw") && 
                config.containsKey("dns")){
                
                if (ip.fromString(config["ip"].as<const char*>()) &&
                    net_mask.fromString(config["mask"].as<const char*>()) &&
                    gw.fromString(config["gw"].as<const char*>()) &&
                    dns.fromString(config["dns"].as<const char*>())){
                    Serial.printf("ifconfig: %s, %s, %s, %s\r\n", ip.toString().c_str(), 
                                                                  net_mask.toString().c_str(), 
                                                                  gw.toString().c_str(), 
                                                                  dns.toString().c_str());
                    dhcp = false;
                }
            }
        }
    }

    if (config.containsKey("volume")){
        volume = config["volume"].as<int>();
        Serial.printf("Volume: %d\r\n", volume);
    }

    if (config.containsKey("relay") && config["relay"].as<bool>()){
        rly_ctl_enabled = true;
        if (config.containsKey("relay_on_time") && config["relay_on_time"].as<int>() != 0){
            rly_on_time = config["relay_on_time"].as<int>();
        }
        Serial.printf("Relay on time: %d\r\n", rly_on_time);
    }
    
}

void init_player(){
    Serial.print(F("Initializing player..."));
    if (player.begin(Serial1)) {
        Serial.print(F("Done\r\n"));
        player.setTimeOut(500);
        player.volume(volume);
        player.EQ(DFPLAYER_EQ_NORMAL);
        player.outputDevice(DFPLAYER_DEVICE_SD);
        player_available = true;
        player.play(1);
    }
    else{
        Serial.print(F("Failed\r\n"));
    }
}

void init_qr_scanner(){
    Serial.print(F("Initializing QR scanner..."));
    if(reader.setup() != SETUP_OK){
        Serial.print("Camera setup failed\r\n");
        return;
    }
    // reader.stream = &stream;
    Serial.print("Setup done...");
    reader.begin();
    Serial.print(F("Done\r\n"));
}

void parse_wifi_qrcode(char *payload){
    Serial.print(F("Parsing WiFi QRCode\r\n"));
    Serial.println(payload);

    MatchState ms(payload);
    char result = ms.Match("WIFI:([STP]):([^;]+);([STP]):([^;]*);([STP]):([^;]*).*;;");
    if (result > 0){
        char *key = (char*)malloc(5);
        char *value = (char*)malloc(100);
        DynamicJsonDocument data(128);
        for (int i = 0; i < 3; i++){
            ms.GetCapture(key, 2 * i);
            ms.GetCapture(value, 2 * i + 1);
            if (strcmp(key, "S") == 0){
                data["ssid"] = value;
            }
            else if (strcmp(key, "P") == 0){
                data["pass"] = value;
            }
        }
        DynamicJsonDocument config(512);
        DynamicJsonDocument keys(512);
        serializeJsonPretty(data, Serial);
        // stop_scanner();
        read_config(config);

        update_value<const char*>(config, data, keys, "ssid");
        update_value<const char*>(config, data, keys, "pass");
        
        if (!write_config(config)){
            Serial.print(F("Failed to save config file\r\n"));
            update_result("خطای ذخیره سازی");
            player.play(4);
        }
        else{
            Serial.print(F("config saved successfully, restart to apply\r\n"));
            update_result("وای فای ذخیره شد");
            player.play(9);
            delay(3000);
            update_result("ریستارت");
            lv_timer_handler();
            player.play(11);
            delay(3000);
            esp_restart();
        }
        // start_scanner();
        free(key);
        free(value);
    }else{
        Serial.println ("Invalid data");
        update_result("نامعتبر!");
        player.play(2);
    }
}

void start_scanner(){
    Serial.print(F("Starting scanner\r\n"));
    reader.pause = false;
    while(reader.paused){
        delay(10);
    }
}

void stop_scanner(){
    Serial.print(F("Stopping scanner\r\n"));
    reader.pause = true;
    while(!reader.paused){
        delay(10);
    }
}

void start_qr_loop(){
    xTaskCreatePinnedToCore(qr_loop, "QRloop", 4096, NULL, 1, &qr_task, 0);
}

int parse_command(const char *payload){
    Serial.print(F("Checking for QR command\r\n"));
    if (strcmp(payload, "###reset###") == 0){
        Serial.print(F("Reset command received\r\n"));
        DynamicJsonDocument config(8);
        deserializeJson(config, "{}");
        if (write_config(config)){
            Serial.print(F("Configuration resetted successfully, restarting the device\r\n"));
            update_result("تنظیمات کارخانه");
            lv_timer_handler();
            player.play(10);
            delay(3000);
            update_result("ریستارت");
            lv_timer_handler();
            player.play(11);
            delay(3000);
            esp_restart();
        }
    }
    else if(strcmp(payload, "###restart###") == 0){
        Serial.print(F("Restart command received, restarting device\r\n"));
        update_result("ریستارت");
        lv_timer_handler();
        player.play(11);
        delay(3000);
        esp_restart();
    }
    return -1;
}

int parse_entry_event(const char *payload){
    Serial.print(F("Parsing event QRCode\r\n"));
    uint32_t now = _now();
    Serial.println(payload);
    DynamicJsonDocument event(128);
    DeserializationError error = deserializeJson(event, payload);
    if (error)  {
        Serial.printf("Invalid code %d\r\n", error);
        return -1;
    }

    if (!event.containsKey("id") ||
        !event.containsKey("lat")  ||
        !event.containsKey("lon")  ||
        !event.containsKey("ts")   ||
        !event.containsKey("st")){
        
        Serial.print(F("required data not available\r\n"));
        update_result("نامعتبر!");
        player.play(2); 
        return 1;
    }

    if (event["id"].as<uint16_t>() <= 0){
        Serial.print(F("Invalid ID\r\n"));
        update_result("نامعتبر!");
        player.play(2); 
        return 1;
    }

    if (event["lat"].as<double>() < -90.0 || event["lat"].as<double>() > 90.0){
        Serial.print(F("Invalid latitude\r\n"));
        update_result("نامعتبر!");
        player.play(2); 
        return 1;
    }

    if (event["lon"].as<double>() < -180.0 || event["lon"].as<double>() > 180.0){
        Serial.print(F("Invalid longitude\r\n"));
        update_result("نامعتبر!");
        player.play(2); 
        return 1;
    }

    if (event["ts"].as<uint32_t>() == 0){
        Serial.print(F("Invalid timestamp\r\n"));
        update_result("نامعتبر!");
        player.play(2); 
        return 1;
    }

    if (event["st"].as<uint8_t>() == 0){
        Serial.print(F("Invalid status\r\n"));
        update_result("نامعتبر!");
        player.play(2); 
        return 1;
    }
    Serial.print(F("Done, checking constarints\r\n"));

    if (time_window != -1 && now - event["ts"].as<uint32_t>() > time_window){
        Serial.printf("Time window closed, delay: %d\r\n", now - event["ts"].as<uint32_t>());
        update_result("منقضی شده");
        player.play(7); 
        return 1;
    }
    // stop_scanner();
    if (lat != -100 && lon -200 && rad != -1){
        double event_lat = event["lat"].as<double>();
        double event_lon = event["lon"].as<double>();
        Serial.printf("evnet loc: (%f, %f)\r\n", event_lat, event_lon);
        double phi1 = event_lat * PI / 180.0;
        double phi2 = lat * PI / 180.0;
        double delta_phi = (lat - event_lat) * PI / 180.0;
        double delta_lmd = (lon - event_lon) * PI / 180.0;

        double a = pow(sin(delta_phi / 2.0), 2.0) + cos(phi1) * cos(phi2) * pow(sin(delta_lmd / 2.0), 2.0);
        double c = 2.0 * atan2(sqrt(a), sqrt(1-a));
        double distance = R * c;
        Serial.printf("distance = %f\r\n", abs(distance));
        if (abs(distance) > rad){
            Serial.printf("Location out of range: %d\r\n", distance);
            update_result("خارج از محدوده");
            player.play(6);
            // start_scanner();
            return 1;
        }
    }

    int rc = insert_event(event["ts"].as<uint32_t>(), event["id"].as<uint16_t>(), event["st"].as<uint8_t>());
    // start_scanner();
    if (rc == SQLITE_OK){
        Serial.print("User aproved\r\n");
        char *name = (char*)malloc(55);
        get_user_name(event["id"].as<uint16_t>(), name);
        update_result(name);
        digitalWrite(relay_pin, HIGH);
        player.play(8);
        lcd_delay(rly_on_time);
        digitalWrite(relay_pin, LOW);
        return 0;
    }
    else if (rc == SQLITE_CONSTRAINT_FOREIGNKEY){
        Serial.print(F("User doesn't exist\r\n"));
        update_result("کاربر یافت نشد");
        player.play(3);
        return 1;
    }
    else if (rc == SQLITE_CONSTRAINT_PRIMARYKEY){
        Serial.print(F("event already exists\r\n"));
        update_result("تکراری");
        player.play(5);
        return 1;
    }
    else{
        Serial.printf("Database Error - %d\r\n", rc);
        update_result("خطا");
        player.play(4);
        return 1;
    }
}

void show_time(){
    char buffer[25];
    if (ntp_enabled){
        sprintf(buffer, "%02d:%02d:%02d", ntp_client->getHours(), ntp_client->getMinutes(), ntp_client->getSeconds());
    }else{
        if (ds_available){
            DateTime now = ds.now();
            sprintf(buffer, "hh:mm:ss");
            now.toString(buffer);
        }else{
            sprintf(buffer, "%02d:%02d:%02d", rtc.getHour(true), rtc.getMinute(), rtc.getSecond());
        }
    }
    lv_label_set_text(time_label, (const char*)buffer);
}

void update_status(const char* text){
    lv_label_set_text(status_label, text);
    lv_timer_handler();
}

void update_result(const char* text){
    lv_label_set_text(result_label, text);
    lv_timer_handler();
}

void setup(){
    Serial.begin(115200);
    Serial.printf("Reset reason: ");
    print_reset_reason();
    Serial.printf("Firmware version: %3.1f\r\n", firmware_version);
    Serial1.begin(9600, 134217756U, 4, 14);
    Wire.begin(13, 15);
    pinMode(relay_pin, OUTPUT);
    digitalWrite(relay_pin, LOW);
    init_display();
    init_fs();
    load_config();
    init_db();
    init_wifi();
    init_time();
    init_player();
    init_qr_scanner();
    xTaskCreatePinnedToCore(qr_loop, "QRloop", 4096, NULL, 1, &qr_task, 0);
    Serial.printf("memory: %u - %u\r\n", ESP.getFreeHeap(), ESP.getFreePsram());
}

void lcd_delay(uint32_t ms){
    uint32_t tm = millis();
    while(millis() - tm < ms){
        lv_timer_handler();
    }
}

void qr_loop(void *parameter){
    update_status("Ready");
    while (true){
        if(reader.receiveQrCode(&qr_code_data, 1)){
            update_status("Proccessing");
            stop_scanner();
            Serial.print(F("Found QRCode: "));
            Serial.printf("payload: %s\r\n", (const char*)qr_code_data.payload);
            if (parse_command((const char*)qr_code_data.payload) == 0){}
            else if (parse_entry_event((const char*)qr_code_data.payload) < 0){
                parse_wifi_qrcode((char*)qr_code_data.payload);
            }
            lcd_delay(2000);
            start_scanner();
            while (reader.receiveQrCode(&qr_code_data, 1));
            // lv_label_cut_text(result_label, 0, strlen(lv_label_get_text(result_label)));
            update_result("");
            update_status("Ready");
        }
        if (millis() - time_timer > 1000){
            time_timer = millis();
            show_time();
        }
        if(ip_label_changed){
            ip_label_changed = false;
            lv_label_set_text(ip_label, ip_label_text);
        }
        lv_timer_handler();
    }
}

void loop(){
    if (webserver_started){
        server.handleClient();
    }
    if (millis() - main_timer > 1000){
        main_timer = millis();
        check_wifi();
        ntp_update();
    }
    if (update_timer != 0 && millis() - update_timer > 5000){
        Serial.printf("Update timedout!\r\n");
        abort_update();
    }
}
