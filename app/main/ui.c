// LVGL VERSION: 9.2.2
// PROJECT: ESP32 LibreLinkUp Client

#include "ui.h"

#define TOTAL_CHART_POINTS                  (142)
///////////////////// VARIABLES ////////////////////

lv_obj_t * ui_Welcome_screen;
lv_obj_t * ui_Main_screen;
lv_obj_t * ui_Debug_screen;
lv_obj_t * ui_FWUpdate_screen;
lv_obj_t * ui_Login_screen;

lv_obj_t * ui_Label_WelcomeInfo;
lv_obj_t * ui_Label_WelcomeWifiInfo;

lv_obj_t * ui_Label_GlucoseValue;
lv_obj_t * ui_Label_GlucoseDelta;
lv_obj_t * ui_Label_GlucoseTrendArrow;
lv_obj_t * ui_Label_GlucoseTrendMessage;

lv_obj_t * ui_Label_LiebreViewAPIActivity;
lv_obj_t * ui_Label_ESP32Connectivity;

lv_obj_t * ui_Label_FWUpdateInfo;
lv_obj_t * ui_Label_FWUpdateProgress_percent;
lv_obj_t * ui_Bar_FWUpdateProgress;

lv_obj_t * ui_Chart_Glucose_5Min;
lv_obj_t * ui_Chart_Valid_Sensor;

lv_obj_t * ui_Label_Chart_GlucoseLimitLow;
lv_obj_t * ui_Label_Chart_GlucoseLimitHigh;

lv_obj_t * ui_Chart_Glucose_5Min_last_point_marker;

lv_obj_t *ui_Chart_x_label_start;
lv_obj_t *ui_Chart_x_label_middle;
lv_obj_t *ui_Chart_x_label_end;

lv_chart_series_t * glucoseValueSeries_upperlimit;
lv_chart_series_t * glucoseValueSeries_lowerlimit;
lv_chart_series_t * glucoseValueSeries_5Min;
lv_chart_series_t * glucoseValueSeries_alert;
lv_chart_series_t * glucoseValueSeries_last;

lv_chart_series_t * sensorValidDaysSeries_yellow;
lv_chart_series_t * sensorValidDaysSeries_grey;

lv_obj_t * ui_Label_DebugInfo;
lv_obj_t * ui_Label_DebugDataRefresh;
lv_obj_t * ui_Label_DebugTime;
lv_obj_t * ui_Label_DebugIP;
lv_obj_t * ui_Label_DebugSensor;
lv_obj_t * ui_Label_DebugSensorTimestamp;
lv_obj_t * ui_Label_DebugSensorState;
lv_obj_t * ui_Label_DebugSensorValue;
lv_obj_t * ui_Label_DebugTest;

lv_obj_t * ui_Label_LoginInfo;
lv_obj_t * ui_kb;
lv_obj_t * ui_ta_email;
lv_obj_t * ui_ta_password;
lv_obj_t * btn_login;
lv_obj_t * btn_label;

lv_obj_t * ui_btn_wireguard;
lv_obj_t * ui_btn_label_wireguard;

lv_obj_t * ui_btn_mqtt;
lv_obj_t * ui_btn_label_mqtt;

lv_obj_t * ui_btn_ota_update;
lv_obj_t * ui_btn_label_ota_update;


///////////////////// TEST LVGL SETTINGS ////////////////////
#if LV_COLOR_DEPTH != 16
    #error "LV_COLOR_DEPTH should be 16bit to match SquareLine Studio's settings"
#endif
/*
#if LV_COLOR_16_SWAP != 1
    #error "LV_COLOR_16_SWAP should be 1 to match SquareLine Studio's settings"
#endif
*/
///////////////////// ANIMATIONS ////////////////////

///////////////////// FUNCTIONS ////////////////////

//int activeMode = 0; // 0 = days, 1 = hours, 2 = minutes

// Erstellt eine neue Blockanzeige für Tage, Stunden oder Minuten
void create_sensor_valid_progress_bar(ProgressBarUI *ui, lv_obj_t *parent, int num_blocks, int x, int y, const char *label_text) {
    ui->total_blocks = num_blocks;

    // Dynamische Blockgröße berechnen (nicht breiter als MAX_BAR_WIDTH)
    int block_size = MAX_BAR_WIDTH / num_blocks;
    if (block_size < 5) block_size = 5;  // Mindestgröße für gute Darstellung
    
    // Berechne die tatsächliche Breite (maximal MAX_BAR_WIDTH)
    int total_width = block_size * num_blocks;
    if (total_width > MAX_BAR_WIDTH) {
        total_width = MAX_BAR_WIDTH;
    }

    // **Hauptcontainer für die Anzeige**
    ui->bar = lv_obj_create(parent);
    lv_obj_set_size(ui->bar, total_width, block_size + 10);
    lv_obj_align(ui->bar, LV_ALIGN_CENTER, x, y);
    lv_obj_set_style_bg_color(ui->bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(ui->bar, 0, 0);
    lv_obj_set_style_pad_all(ui->bar, 5, 0);

    // **Blöcke innerhalb des Containers erstellen**
    for (int i = 0; i < num_blocks; i++) {
        ui->blocks[i] = lv_obj_create(ui->bar);
        lv_obj_set_size(ui->blocks[i], block_size - 2, block_size - 2); // Dynamische Größe
        lv_obj_set_style_bg_color(ui->blocks[i], lv_color_make(80, 80, 80), 0); // Standard: Grau
        lv_obj_set_style_radius(ui->blocks[i], 2, 0);
        lv_obj_set_style_border_width(ui->blocks[i], 0, 0);
        lv_obj_align(ui->blocks[i], LV_ALIGN_LEFT_MID, i * block_size, 0);
    }

    // **Textlabel für die Anzeige**
    ui->label = lv_label_create(parent);
    lv_label_set_text(ui->label, label_text);
    lv_obj_set_style_text_font(ui->label, &JetBrainsMonoLight16, 0);
    lv_obj_set_style_text_color(ui->label, lv_color_white(), 0);
    lv_obj_align(ui->label, LV_ALIGN_CENTER, x, y + block_size + 10);
}

// Aktualisiert die Anzeige mit den verbleibenden Blöcken
void update_sensor_valid_progress_bar(ProgressBarUI *ui, int remaining) {
    if (remaining > ui->total_blocks) remaining = ui->total_blocks;
    if (remaining < 0) remaining = 0;

    // Farben setzen
    for (int i = 0; i < ui->total_blocks; i++) {
        if (i < remaining) {
            if(ui->total_blocks == BLOCKS_VALID_DAYS){
                lv_obj_set_style_bg_color(ui->blocks[i], lv_color_make(100, 200, 100), 0); // reen
            }
            else if(ui->total_blocks == BLOCKS_VALID_DAYS && remaining <= 3){ // last 3 days
                lv_obj_set_style_bg_color(ui->blocks[i], lv_color_make(200, 200, 0), 0); // yellow
            }
            else{
                lv_obj_set_style_bg_color(ui->blocks[i], lv_color_make(200, 0, 0), 0); // red
            }
            
        } else {
            lv_obj_set_style_bg_color(ui->blocks[i], lv_color_make(80, 80, 80), 0); // Grau
        }
    }

    // set label text
    lv_label_set_text_fmt(ui->label, " ");
    
    //lv_timer_handler();

    if(ui->total_blocks == BLOCKS_VALID_DAYS){
        lv_label_set_text_fmt(ui->label, "Sensor exp. in %d days", remaining);
    }else if(ui->total_blocks == BLOCKS_VALID_HOURS){
        lv_label_set_text_fmt(ui->label, "Sensor exp. in %d hours", remaining);
    }else if(ui->total_blocks == BLOCKS_VALID_MINUTES){
        lv_label_set_text_fmt(ui->label, "Sensor exp. in %d minutes", remaining);
    }
}

// **Schaltet zwischen den Progress Bars um**
void switch_sensor_valid_progress_bar(ProgressBarUI *ui, int mode) {
    
    // clear label text
    lv_label_set_text_fmt(ui->label, "");
    
    // Alle ausblenden
    lv_obj_add_flag(dayBar.bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(hourBar.bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(minuteBar.bar, LV_OBJ_FLAG_HIDDEN);
    
    // Die gewünschte Progress Bar sichtbar machen
    if (ui->bar == dayBar.bar) {
        lv_obj_clear_flag(dayBar.bar, LV_OBJ_FLAG_HIDDEN);
    } else if (ui->bar == hourBar.bar) {
        lv_obj_clear_flag(hourBar.bar, LV_OBJ_FLAG_HIDDEN);
    } else if (ui->bar == minuteBar.bar) {
        lv_obj_clear_flag(minuteBar.bar, LV_OBJ_FLAG_HIDDEN);
    }
}

// **Erstellt alle drei Progress Bars, zeigt aber nur eine an**
void create_all_sensor_valid_progress_bars() {
    create_sensor_valid_progress_bar(&dayBar, ui_Main_screen,    BLOCKS_VALID_DAYS, 0, 170, "");
    create_sensor_valid_progress_bar(&hourBar, ui_Main_screen,   BLOCKS_VALID_HOURS, 0, 170, "");
    create_sensor_valid_progress_bar(&minuteBar, ui_Main_screen, BLOCKS_VALID_MINUTES, 0, 170, "");

    switch_sensor_valid_progress_bar(&dayBar, 0); // Standard: Tage
}

// **Update-Funktion, um die aktuelle Progress Bar zu aktualisieren**
void update_chart_valid_values(ProgressBarUI *ui, int value) {
    if (ui->bar == dayBar.bar) {
        update_sensor_valid_progress_bar(&dayBar, value);
    } else if (ui->bar == hourBar.bar) {
        update_sensor_valid_progress_bar(&hourBar, value);
    } else if (ui->bar == minuteBar.bar) {
        update_sensor_valid_progress_bar(&minuteBar, value);
    }
}
///////////////////// SCREENS ////////////////////
void ui_Welcome_screen_init(void)
{
    ui_Welcome_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Welcome_screen, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Welcome_screen, lv_color_black(), LV_PART_MAIN);

    ui_Label_WelcomeInfo = lv_label_create(ui_Welcome_screen);
    lv_obj_set_style_text_color(ui_Label_WelcomeInfo, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_WelcomeInfo, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_WelcomeInfo, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_WelcomeInfo, &JetBrainsMonoLight56, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_WelcomeInfo, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(ui_Label_WelcomeInfo, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_WelcomeInfo, LV_ALIGN_CENTER, 0, -60);

    ui_Label_WelcomeWifiInfo = lv_label_create(ui_Welcome_screen);
    lv_obj_set_style_text_color(ui_Label_WelcomeWifiInfo, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_WelcomeWifiInfo, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_WelcomeWifiInfo, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_WelcomeWifiInfo, &JetBrainsMonoLight24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(ui_Label_WelcomeWifiInfo, 400);                               
    lv_obj_set_style_text_align(ui_Label_WelcomeWifiInfo, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui_Label_WelcomeWifiInfo, LV_ALIGN_CENTER, 0, 150);
    lv_label_set_text(ui_Label_WelcomeWifiInfo, "" );
}

void ui_Main_screen_init(void)
{
    ui_Main_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Main_screen, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Main_screen, lv_color_black(), LV_PART_MAIN);

    ui_Label_GlucoseValue = lv_label_create(ui_Main_screen);
    lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_GlucoseValue, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_GlucoseValue, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_GlucoseValue, &JetBrainsMonoLight100, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_GlucoseValue, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(ui_Label_GlucoseValue, 200);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_GlucoseValue, LV_ALIGN_CENTER, 0, -140);
    lv_label_set_text(ui_Label_GlucoseValue, "" );

    ui_Label_GlucoseDelta = lv_label_create(ui_Main_screen);
    lv_obj_set_style_text_color(ui_Label_GlucoseDelta, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_GlucoseDelta, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_GlucoseDelta, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_GlucoseDelta, &JetBrainsMonoLight36, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(ui_Label_GlucoseDelta, 300);                               
    lv_obj_set_style_text_align(ui_Label_GlucoseDelta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui_Label_GlucoseDelta, LV_ALIGN_CENTER, 0, -60);
    lv_label_set_text(ui_Label_GlucoseDelta, "" );
    
    ui_Label_GlucoseTrendArrow = lv_label_create(ui_Main_screen);
    lv_obj_set_style_text_color(ui_Label_GlucoseTrendArrow, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_GlucoseTrendArrow, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_GlucoseTrendArrow, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_GlucoseTrendArrow, &JetBrainsMonoLight72, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(ui_Label_GlucoseTrendArrow, 100);                               /*Set smaller width to make the lines wrap*/
    lv_obj_set_style_text_align(ui_Label_GlucoseTrendArrow, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui_Label_GlucoseTrendArrow, LV_ALIGN_CENTER, 130, -100);
    lv_label_set_text(ui_Label_GlucoseTrendArrow, "" );

    ui_Label_GlucoseTrendMessage = lv_label_create(ui_Main_screen);
    lv_obj_set_style_text_color(ui_Label_GlucoseTrendMessage, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_GlucoseTrendMessage, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_GlucoseTrendMessage, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_GlucoseTrendMessage, &JetBrainsMonoLight36, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(ui_Label_GlucoseTrendMessage, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_set_style_text_align(ui_Label_GlucoseTrendMessage, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui_Label_GlucoseTrendMessage, LV_ALIGN_CENTER, 0, 55);
    lv_label_set_text(ui_Label_GlucoseTrendMessage, "" );

    ui_Label_LiebreViewAPIActivity = lv_label_create(ui_Main_screen);
    lv_obj_set_style_text_color(ui_Label_LiebreViewAPIActivity, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_LiebreViewAPIActivity, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_LiebreViewAPIActivity, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_LiebreViewAPIActivity, &JetBrainsMonoLight36, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(ui_Label_LiebreViewAPIActivity, 20);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_LiebreViewAPIActivity, LV_ALIGN_CENTER, 100, -180);
    lv_label_set_text(ui_Label_LiebreViewAPIActivity, " " );

    ui_Label_ESP32Connectivity = lv_label_create(ui_Main_screen);
    lv_obj_set_style_text_color(ui_Label_ESP32Connectivity, lv_color_hex(0x00FF00), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_ESP32Connectivity, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_ESP32Connectivity, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_ESP32Connectivity, &JetBrainsMonoLight24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(ui_Label_ESP32Connectivity, 20);
    lv_obj_align(ui_Label_ESP32Connectivity, LV_ALIGN_CENTER, 100, -180);
    lv_label_set_text(ui_Label_ESP32Connectivity, " " );
    
    ui_Label_Chart_GlucoseLimitHigh = lv_label_create(ui_Main_screen);
    lv_obj_set_style_text_color(ui_Label_Chart_GlucoseLimitHigh, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_Chart_GlucoseLimitHigh, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_Chart_GlucoseLimitHigh, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_Chart_GlucoseLimitHigh, &JetBrainsMonoLight16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(ui_Label_Chart_GlucoseLimitHigh, 40);                               /*Set smaller width to make the lines wrap*/
    lv_obj_set_style_text_align(ui_Label_Chart_GlucoseLimitHigh, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui_Label_Chart_GlucoseLimitHigh, LV_ALIGN_CENTER, -175, 0);
    lv_label_set_text(ui_Label_Chart_GlucoseLimitHigh, "" );

    ui_Label_Chart_GlucoseLimitLow = lv_label_create(ui_Main_screen);
    lv_obj_set_style_text_color(ui_Label_Chart_GlucoseLimitLow, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_Chart_GlucoseLimitLow, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_Chart_GlucoseLimitLow, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_Chart_GlucoseLimitLow, &JetBrainsMonoLight16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(ui_Label_Chart_GlucoseLimitLow, 40);                               /*Set smaller width to make the lines wrap*/
    lv_obj_set_style_text_align(ui_Label_Chart_GlucoseLimitLow, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui_Label_Chart_GlucoseLimitLow, LV_ALIGN_CENTER, -180, 95);
    lv_label_set_text(ui_Label_Chart_GlucoseLimitLow, "" );

    ui_Chart_Glucose_5Min = lv_chart_create(ui_Main_screen);
    lv_obj_set_size(ui_Chart_Glucose_5Min, 400, 200);
    lv_obj_align(ui_Chart_Glucose_5Min, LV_ALIGN_CENTER, 0, 50);
    lv_obj_set_style_size(ui_Chart_Glucose_5Min, 1, 1 , LV_PART_INDICATOR);
	lv_obj_set_style_line_width(ui_Chart_Glucose_5Min, 5, LV_PART_ITEMS);
	lv_obj_set_style_border_width(ui_Chart_Glucose_5Min, 0, LV_PART_MAIN);
	lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(ui_Chart_Glucose_5Min, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_chart_set_update_mode(ui_Chart_Glucose_5Min, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_range(ui_Chart_Glucose_5Min, LV_CHART_AXIS_PRIMARY_Y , 40, 225);
    lv_chart_set_point_count(ui_Chart_Glucose_5Min, TOTAL_CHART_POINTS);        //starts like an array with "0"
    lv_chart_set_type(ui_Chart_Glucose_5Min, LV_CHART_TYPE_LINE);       /*Show lines and points too*/
    lv_chart_set_div_line_count(ui_Chart_Glucose_5Min, 5, 5);           // background chart lines
    lv_obj_clear_flag(ui_Chart_Glucose_5Min, LV_OBJ_FLAG_SCROLLABLE);
    
    /*Add five data series*/
    glucoseValueSeries_upperlimit = lv_chart_add_series(ui_Chart_Glucose_5Min, lv_palette_main(LV_PALETTE_ORANGE), LV_CHART_AXIS_PRIMARY_Y);
    glucoseValueSeries_lowerlimit = lv_chart_add_series(ui_Chart_Glucose_5Min, lv_palette_main(LV_PALETTE_ORANGE), LV_CHART_AXIS_PRIMARY_Y);
    glucoseValueSeries_5Min       = lv_chart_add_series(ui_Chart_Glucose_5Min, lv_palette_main(LV_PALETTE_GREEN), LV_CHART_AXIS_PRIMARY_Y);
    glucoseValueSeries_alert      = lv_chart_add_series(ui_Chart_Glucose_5Min, lv_palette_main(LV_PALETTE_RED), LV_CHART_AXIS_PRIMARY_Y);
    glucoseValueSeries_last       = lv_chart_add_series(ui_Chart_Glucose_5Min, lv_palette_main(LV_PALETTE_YELLOW), LV_CHART_AXIS_PRIMARY_Y);

    // circle for last graph value
    ui_Chart_Glucose_5Min_last_point_marker = lv_obj_create(ui_Chart_Glucose_5Min);
    lv_obj_add_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN); // hide object
    lv_obj_set_size(ui_Chart_Glucose_5Min_last_point_marker, 15, 15);
    lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min_last_point_marker, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_radius(ui_Chart_Glucose_5Min_last_point_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(ui_Chart_Glucose_5Min_last_point_marker, lv_color_white(), 0);
    lv_obj_set_style_border_width(ui_Chart_Glucose_5Min_last_point_marker, 2, 0); 

    // create X-Axis label
    ui_Chart_x_label_start = lv_label_create(ui_Main_screen);
    lv_label_set_text(ui_Chart_x_label_start, "");
    lv_obj_set_pos(ui_Chart_x_label_start, 50, 375);

    ui_Chart_x_label_middle = lv_label_create(ui_Main_screen);
    lv_label_set_text(ui_Chart_x_label_middle, "");
    lv_obj_set_pos(ui_Chart_x_label_middle, 225, 375);

    ui_Chart_x_label_end = lv_label_create(ui_Main_screen);
    lv_label_set_text(ui_Chart_x_label_end, "");
    lv_obj_set_pos(ui_Chart_x_label_end, 395, 375);

    //Add data to sensor valid chart
    create_all_sensor_valid_progress_bars();

}

void ui_Debug_screen_init(void)
{
    ui_Debug_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Debug_screen, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Debug_screen, lv_color_black(), LV_PART_MAIN);
    
    ui_Label_DebugInfo = lv_label_create(ui_Debug_screen);
    lv_obj_set_style_text_color(ui_Label_DebugInfo, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_DebugInfo, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_DebugInfo, &JetBrainsMonoLight32, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_DebugInfo, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(ui_Label_DebugInfo, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_DebugInfo, LV_TEXT_ALIGN_CENTER, 100, 30);
    lv_label_set_text(ui_Label_DebugInfo,"Debug Screen" );

    ui_Label_DebugDataRefresh = lv_label_create(ui_Debug_screen);
    lv_obj_set_style_text_color(ui_Label_DebugDataRefresh, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_DebugDataRefresh, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_DebugDataRefresh, &JetBrainsMonoLight20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_DebugDataRefresh, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(ui_Label_DebugDataRefresh, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_DebugDataRefresh, LV_TEXT_ALIGN_CENTER, 10, 100);
    lv_label_set_text(ui_Label_DebugDataRefresh, "DataRefreshIn: " );

    ui_Label_DebugIP = lv_label_create(ui_Debug_screen);
    lv_obj_set_style_text_color(ui_Label_DebugIP, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_DebugIP, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_DebugIP, &JetBrainsMonoLight20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_DebugIP, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(ui_Label_DebugIP, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_DebugIP, LV_TEXT_ALIGN_CENTER, 10, 120);
    lv_label_set_text(ui_Label_DebugIP, "IP: " );

    ui_Label_DebugTime = lv_label_create(ui_Debug_screen);
    lv_obj_set_style_text_color(ui_Label_DebugTime, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_DebugTime, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_DebugTime, &JetBrainsMonoLight20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_DebugTime, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(ui_Label_DebugTime, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_DebugTime, LV_TEXT_ALIGN_CENTER, 10, 140);
    lv_label_set_text(ui_Label_DebugTime, "ESP32 Time: " );

    ui_Label_DebugSensor = lv_label_create(ui_Debug_screen);
    lv_obj_set_style_text_color(ui_Label_DebugSensor, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_DebugSensor, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_DebugSensor, &JetBrainsMonoLight20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_DebugSensor, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(ui_Label_DebugSensor, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_DebugSensor, LV_TEXT_ALIGN_CENTER, 10, 160);
    lv_label_set_text(ui_Label_DebugSensor, "Sensor: " );

    ui_Label_DebugSensorTimestamp = lv_label_create(ui_Debug_screen);
    lv_obj_set_style_text_color(ui_Label_DebugSensorTimestamp, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_DebugSensorTimestamp, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_DebugSensorTimestamp, &JetBrainsMonoLight20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_DebugSensorTimestamp, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(ui_Label_DebugSensorTimestamp, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_DebugSensorTimestamp, LV_TEXT_ALIGN_CENTER, 10, 200);
    lv_label_set_text(ui_Label_DebugSensorTimestamp, "SensorTimestamp: " );

    ui_Label_DebugSensorState = lv_label_create(ui_Debug_screen);
    lv_obj_set_style_text_color(ui_Label_DebugSensorState, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_DebugSensorState, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_DebugSensorState, &JetBrainsMonoLight20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_DebugSensorState, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(ui_Label_DebugSensorState, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_DebugSensorState, LV_TEXT_ALIGN_CENTER, 10, 220);
    lv_label_set_text(ui_Label_DebugSensorState, "SensorState: " );

    ui_Label_DebugSensorValue = lv_label_create(ui_Debug_screen);
    lv_obj_set_style_text_color(ui_Label_DebugSensorValue, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_DebugSensorValue, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_DebugSensorValue, &JetBrainsMonoLight20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_DebugSensorValue, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(ui_Label_DebugSensorValue, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_DebugSensorValue, LV_TEXT_ALIGN_CENTER, 10, 240);
    lv_label_set_text(ui_Label_DebugSensorValue, "SensorState: " );

    ui_Label_DebugTest = lv_label_create(ui_Debug_screen);
    lv_obj_set_style_text_color(ui_Label_DebugTest, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_DebugTest, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_DebugTest, &JetBrainsMonoLight72, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_DebugTest, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(ui_Label_DebugTest, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_DebugTest, LV_TEXT_ALIGN_CENTER, 20, 280);
    lv_label_set_text(ui_Label_DebugTest, "↓ ↘️ → ↗️ ↑" );

}

void ui_Login_screen_init(void){
    ui_Login_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Login_screen, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Login_screen, lv_color_black(), LV_PART_MAIN);

    ui_Label_LoginInfo = lv_label_create(ui_Login_screen);
    lv_obj_set_style_text_color(ui_Label_LoginInfo, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_LoginInfo, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_LoginInfo, &JetBrainsMonoLight32, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_LoginInfo, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(ui_Label_LoginInfo, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_LoginInfo, LV_TEXT_ALIGN_CENTER, 120, 30);
    lv_label_set_text(ui_Label_LoginInfo,"LLU Login" );

    /*Create a keyboard to use it with an of the text areas*/
    ui_kb = lv_keyboard_create(ui_Login_screen);
    lv_obj_set_size(ui_kb, 400, 180);
    lv_obj_align(ui_kb, LV_ALIGN_CENTER, 0, 40);

    /*Create a text area. The keyboard will write here*/
    ui_ta_email = lv_textarea_create(ui_Login_screen);
    lv_obj_align(ui_ta_email, LV_TEXT_ALIGN_CENTER, 0, 80);
    lv_obj_set_style_text_font(ui_ta_email, &JetBrainsMonoLight16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_textarea_set_placeholder_text(ui_ta_email, "user@example.com");
    lv_obj_set_size(ui_ta_email, 200, 40);

    /*Create a text area. The keyboard will write here*/
    ui_ta_password = lv_textarea_create(ui_Login_screen);
    lv_obj_align(ui_ta_password, LV_TEXT_ALIGN_CENTER, 0, 140);
    lv_obj_set_style_text_font(ui_ta_password, &JetBrainsMonoLight16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_textarea_set_placeholder_text(ui_ta_password, "password");
    lv_textarea_set_password_mode(ui_ta_password, true);
    lv_obj_set_size(ui_ta_password, 200, 40);

    // Login-Button erstellen
    btn_login = lv_btn_create(ui_Login_screen);
    lv_obj_set_size(btn_login, 100, 50);
    lv_obj_align(btn_login, LV_ALIGN_CENTER, 0, 180);

    btn_label = lv_label_create(btn_login);
    lv_label_set_text(btn_label, "Login");
    lv_obj_center(btn_label);
}

void ui_FWUpdate_screen_init(void)
{
    ui_FWUpdate_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_FWUpdate_screen, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_FWUpdate_screen, lv_color_black(), LV_PART_MAIN);

    ui_Label_FWUpdateInfo = lv_label_create(ui_FWUpdate_screen);
    lv_obj_set_style_text_color(ui_Label_FWUpdateInfo, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_FWUpdateInfo, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_FWUpdateInfo, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_FWUpdateInfo, &JetBrainsMonoLight36, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_FWUpdateInfo, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(ui_Label_FWUpdateInfo, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_FWUpdateInfo, LV_ALIGN_CENTER, 0, -100);
    lv_label_set_text(ui_Label_FWUpdateInfo, "FW Update ..." );

    // Fortschrittsbalken erstellen
    /*
    ui_Bar_FWUpdateProgress = lv_bar_create(ui_FWUpdate_screen);
    lv_obj_set_size(ui_Bar_FWUpdateProgress, 400, 60);  // Größe des Balkens
    lv_obj_align(ui_Bar_FWUpdateProgress, LV_ALIGN_CENTER, 0, 100);  // Position
    lv_bar_set_range(ui_Bar_FWUpdateProgress, 0, 100);  // Wertebereich 0 bis 100
    // Stil für den Indikator (den Fortschrittsbalken) setzen
    lv_obj_set_style_bg_color(ui_Bar_FWUpdateProgress, lv_palette_main(LV_PALETTE_RED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ui_Bar_FWUpdateProgress, LV_OPA_COVER, LV_PART_INDICATOR);
    // Stil für den Hintergrund (eckig machen)
    lv_obj_set_style_radius(ui_Bar_FWUpdateProgress, 0, LV_PART_MAIN);  // Kein Radius für den Hintergrund
    // Stil für den Indikator (eckig machen)
    lv_obj_set_style_radius(ui_Bar_FWUpdateProgress, 0, LV_PART_INDICATOR);  // Kein Radius für den Fortschrittsbalken
    lv_obj_set_style_bg_color(ui_Bar_FWUpdateProgress, lv_palette_main(LV_PALETTE_RED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ui_Bar_FWUpdateProgress, LV_OPA_COVER, LV_PART_INDICATOR);
    */

    // Prozentanzeige innerhalb des Fortschrittsbalkens
    ui_Label_FWUpdateProgress_percent = lv_label_create(ui_FWUpdate_screen);
    lv_obj_set_style_text_color(ui_Label_FWUpdateProgress_percent, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label_FWUpdateProgress_percent, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_FWUpdateProgress_percent, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label_FWUpdateProgress_percent, &JetBrainsMonoLight72, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Label_FWUpdateProgress_percent, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(ui_Label_FWUpdateProgress_percent, 400);                               /*Set smaller width to make the lines wrap*/
    lv_obj_align(ui_Label_FWUpdateProgress_percent, LV_ALIGN_CENTER, 0, 50);
    lv_label_set_text(ui_Label_FWUpdateProgress_percent, "0%");
    
    //lv_obj_align_to(ui_Label_FWUpdateProgress_percent, ui_Bar_FWUpdateProgress, LV_ALIGN_CENTER, 0, 0);  // Zentriert im Balken
}

void ui_btn_debug_screen_init(lv_obj_t * parent){
//WireGuard
    ui_btn_wireguard = lv_btn_create(parent);
    lv_obj_align(ui_btn_wireguard, LV_ALIGN_CENTER, -115, 150);
    lv_obj_add_flag(ui_btn_wireguard, LV_OBJ_FLAG_CHECKABLE);

    ui_btn_label_wireguard = lv_label_create(ui_btn_wireguard);
    lv_obj_center(ui_btn_label_wireguard);
    lv_label_set_text(ui_btn_label_wireguard, "WG");

//MQTT
    ui_btn_mqtt = lv_btn_create(parent);
    lv_obj_align(ui_btn_mqtt, LV_ALIGN_CENTER, 5, 150);
    lv_obj_add_flag(ui_btn_mqtt, LV_OBJ_FLAG_CHECKABLE);    
    
    ui_btn_label_mqtt = lv_label_create(ui_btn_mqtt);
    lv_obj_center(ui_btn_label_mqtt);
    lv_label_set_text(ui_btn_label_mqtt, "MQTT");

//OTA Update
    ui_btn_ota_update = lv_btn_create(parent);
    lv_obj_align(ui_btn_ota_update, LV_ALIGN_CENTER, 130, 150);
    lv_obj_add_flag(ui_btn_ota_update, LV_OBJ_FLAG_CHECKABLE);    
    
    ui_btn_label_ota_update = lv_label_create(ui_btn_ota_update);
    lv_obj_center(ui_btn_label_ota_update);
    lv_label_set_text(ui_btn_label_ota_update, "OTA");
}

//----------------[Init]-----------------------

void ui_init(void)
{
    lv_disp_t * dispp = lv_disp_get_default();
    lv_theme_t * theme = lv_theme_default_init(dispp, lv_palette_main(LV_PALETTE_GREY), lv_palette_main(LV_PALETTE_GREY), true, LV_FONT_DEFAULT);
    
    lv_disp_set_theme(dispp, theme);
    ui_Welcome_screen_init();
    ui_Main_screen_init();
    ui_Debug_screen_init();
    ui_Login_screen_init();
    ui_FWUpdate_screen_init();
    //.... more screen inits...
    
    //.... init buttons 
    ui_btn_debug_screen_init(ui_Debug_screen);

    lv_disp_load_scr(ui_Welcome_screen);
}