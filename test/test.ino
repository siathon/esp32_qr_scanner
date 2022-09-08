#include <Regexp.h>
#include <ArduinoJson.h>
#include <sqlite3.h>
#include <LittleFS.h>
#include <ESP32Time.h>
#include <Wire.h>
#include <lvgl.h>
#include <Adafruit_SSD1306.h>

#define FORMAT_LITTLEFS_IF_FAILED true

char *zErrMsg;
sqlite3 *db;
ESP32Time rtc;
int relay_pin = 2;

static const uint16_t screenWidth  = 128;
static const uint16_t screenHeight = 64;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[ screenWidth * 10 ];

Adafruit_SSD1306 display(128, 64, &Wire, -1, 800000UL, 400000UL);

void test_regex(){
    char buf [100] = "WIFI:S:Saymantech;T:;P:;;";

    // match state object
    MatchState ms (buf);

    Serial.println (buf);
    char result = ms.Match("WIFI:([STP]):([^;]+);([STP]):([^;]*);([STP]):([^;]*).*;;");

    if (result > 0){
        char cap [10];
        for (byte i = 0; i < ms.level; i++){
            Serial.print ("Capture "); 
            Serial.print (i, DEC);
            Serial.print (" = ");
            ms.GetCapture (cap, i);
            Serial.println (cap); 
        }
    }else{
        Serial.println ("No match.");
    }
}

static int select_callback(void *data, int argc, char **argv, char **col_name){
    for (int i = 0; i < argc; i++){
        Serial.printf("%s = %s\r\n", col_name[i], argv[i] ? argv[i] : "NULL");
    }
    return 0;
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
    int rc = sqlite3_open("/fs/database.db", &db);
    if (rc){
        Serial.printf("Failed: %s\r\n", sqlite3_errstr(rc));
        return -1;
    }
    Serial.print(F("Done\r\n"));

    Serial.print(F("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT);..."));
    rc = sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT);", NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        sqlite3_free(zErrMsg);
        return -2;
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

int insert(int id, const char* name){
    char *query = (char*)malloc(256);
    sprintf(query, "INSERT INTO users VALUES (%d, '%s');", id, name);
    Serial.printf("%s...", query);
    int rc = sqlite3_exec(db, query, NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        return rc;
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

int select(){
    char *query;
    char* data;
    query = (char*)malloc(250);
    data = (char*)malloc(10);
    sprintf(query, "SELECT * FROM users;");
    Serial.println(query);
    int rc = sqlite3_exec(db, query, select_callback, (void*)data, &zErrMsg);
    if (rc != SQLITE_OK){
        Serial.printf("SQL error: %s\r\n", zErrMsg);
        return rc;
    }
    Serial.print(F("Done\r\n"));
    return 0;
}

// void test_db_double(){
//     init_fs();
//     init_db();
//     double a = -89.987654;
//     double b = 89.987654;
//     double c = -179.987654;
//     double d = 179.987654;
//     insert(0, a);
//     insert(1, b);
//     insert(2, c);
//     insert(3, d);
//     select();
// }

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

void test_lcd_lvgl(){
    Wire.begin(13, 15);
    String LVGL_Arduino = "Hello Arduino! ";
    LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();

    Serial.println( LVGL_Arduino );
    Serial.println( "I am LVGL_Arduino" );

    lv_init();

    display.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false);
    display.clearDisplay();
    display.display();

    lv_disp_draw_buf_init( &draw_buf, buf, NULL, screenWidth * 10 );

    /*Initialize the display*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init( &disp_drv );
    /*Change the following line to your display resolution*/
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register( &disp_drv );

    lv_obj_t *btn = lv_btn_create(lv_scr_act());
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_size(btn, 50, 30);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_size(label, 50, 30);
    lv_obj_set_style_text_font(label, &lv_font_dejavu_16_persian_hebrew, 0);
    lv_obj_set_style_base_dir(label, LV_BASE_DIR_RTL, 0);
    lv_label_set_text( label, "فشار بده" );
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_center(label);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);

    // void loop(){
    //     lv_timer_handler();
    //     delay(5);
    // }

}

void setup(){
    Serial.begin(115200);
    pinMode(relay_pin, OUTPUT);
}

void loop(){
    digitalWrite(relay_pin, HIGH);
    delay(1000);
    digitalWrite(relay_pin, LOW);
    delay(1000);
}