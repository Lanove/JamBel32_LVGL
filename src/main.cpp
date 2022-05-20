// Screen size is 480*320 (Pixel)
#include <Arduino.h>
#include <globals.h>
#include <map>
#include <string.h>
#include <SPI.h>
#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include <pcf8574.h>
#include <plc_timer.h>


RTC_DS3231* rtc;
SPIClass SDSPI;
AudioGeneratorMP3* mp3PCM;
AudioFileSourceSD* mp3Source;
AudioOutputI2S* i2sOut;
pcf8574* ioExpander;

void audioTask_cb(void* pvParameters);
TaskHandle_t audioTask;
DateTime now;

bool rtcBeginFailFlag, rtcPowerLostFlag, sdNotDetectedFlag;
bool sdBeginFlag;

bool bellPlayedFlag = false; // Flag used so JadwalBel audio is only played once, not repeated
int bellPlayedIndex = 0;
bool isAudioPlaying = false, // Signal from core 0, is audio playing?
preAudioPlay = false, // Used for giving 2000ms delay after turning on relay and before playing audio file
audioPlayFlag = false, // Signal from core 1 to core 0 to play the audiofile on path audioPath[256], cleared by core 0
stopAudio = false; // Signal from core 0 to core 1 to turn off relay, cleared by core 1
char audioPath[256] = { 0 };
SemaphoreHandle_t audioMutex;

bool wifiConnected;

void setup(void) {

  if (ESP.getEfuseMac() != DEVICE_MAC) {
    delay(1000);
    *((char*)0) = 0; // restart by segfault if device is illegal
  }

  btStop();
  jw_used = new JadwalHari();
  jw_temp = new JadwalHari();
  tj_lists = (TemplateJadwal*)malloc(sizeof(TemplateJadwal) * TJ_MAX_LEN);
  rtc = new RTC_DS3231();
  mp3PCM = new AudioGeneratorMP3();
  mp3Source = new AudioFileSourceSD();
  i2sOut = new AudioOutputI2S();
  ioExpander = new pcf8574();

  Serial.begin(115200);
  log_i("SDK %s", ESP.getSdkVersion());
  log_i("Release v%s", RELEASE_VER);
  log_i("CPU Freq : %luMHz", ESP.getCpuFreqMHz());
  // log_i("Chip Revision %d Chip Model : %s Chip Core : %d", ESP.getChipRevision(), ESP.getChipModel(), ESP.getChipCores());
  // log_i("Flash Size : %luKB Flash Speed : %lu Flash Mode : %s", ESP.getFlashChipSize() / 1024, ESP.getFlashChipSpeed(), VERBOSE_FLASH_MODE(ESP.getFlashChipMode()));
  // log_i("Sketch MD5: %s Sketch Size : %luKB Free Sketch Space : %luKB Heap Size : %luKB Free Heap : %luKB", ESP.getSketchMD5().c_str(), ESP.getSketchSize() / 1024, ESP.getFreeSketchSpace() / 1024, ESP.getHeapSize() / 1024, ESP.getFreeHeap() / 1024);

  lv_init();
  lgfx_init();
  lvgl_esp32_init();
  initStyles();

  SDSPI.begin(SD_CLK, SD_DO, SD_DI, SD_CS);
  SDSPI.setFrequency(SDSPI_FREQUENCY);
  sdBeginFlag = SD.begin(SD_CS, SDSPI, SDSPI_FREQUENCY);
  if (sdBeginFlag)
    log_d("SD Card Detected! Card Type : %s Card Size : %lluMB Total Bytes : %lluMB Used Bytes : %lluMB", VERBOSE_SD_TYPE(SD.cardType()), SD.cardSize() / (1024 * 1024), SD.totalBytes() / (1024 * 1024), SD.usedBytes() / (1024 * 1024));
  else {
    log_d("SD.begin() failed");
    sdNotDetectedFlag = true;
  }

  // Uncomment following line if ESPSYS_FS is not SD
  // log_d("Inizializing FS...\n");
  // if (ESPSYS_FS.begin())
  //   log_d("FS OK!\nTotal Bytes : %llu\nUsed Bytes : %llu\nFree Bytes : %llu\n", ESPSYS_FS.totalBytes(), ESPSYS_FS.usedBytes(), (ESPSYS_FS.totalBytes() - ESPSYS_FS.usedBytes()));
  // else
  //   log_d("FS Fail!\n");

  Wire.begin(int(I2C_SDA), int(I2C_SCL), I2C_FREQ);
  rtcBeginFailFlag = !rtc->begin();
  if (rtcBeginFailFlag)
    log_e("RTC not found!");
  rtcPowerLostFlag = rtc->lostPower();

  now = rtc->now();
  volume_load();
  belManual_load(belManual, belManual_len);
  templateJadwal_activeName_load();
  templateJadwal_list_load();
  jadwalHari_load(&tj_used, jw_used, tj_used.tipeJadwal == TJ_MINGGUAN ? now.dayOfTheWeek() : 0);

  i2sOut->SetPinout(I2S_BCK, I2S_WS, I2S_DO);
  i2sOut->SetGain(volumeToGain(audioVolume));

  ioExpander->init(IOEXPAND_I2C_ADDRESS);
  ioExpander->writeByte(0x00);

  audioMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(
    audioTask_cb,   /* Task function. */
    "audioTask",     /* name of task. */
    10000,       /* Stack size of task */
    NULL,        /* parameter of the task */
    1,           /* priority of the task */
    &audioTask,      /* Task handle to keep track of created task */
    0);          /* pin task to core 0 */
  log_i("Heap Size : %luKB\nFree Heap : %luKB", ESP.getHeapSize() / 1024, ESP.getFreeHeap() / 1024);

  loadMainScreen();
}

unsigned long lastRTCMillis;
uint8_t lastSecond, lastDay;
bool stled_status = false;
TON timerDelayStart(2000);
TON timerDelayStop(2000);

void loop() {

  if (millis() - lastRTCMillis >= 1000) {
    lastRTCMillis = millis();
    now = rtc->now();
    ioExpander->write(Expander::ST_LED, stled_status);
    stled_status = !stled_status;
  }

  if (lastSecond != now.second()) {
    if (lv_scr_act() == mainScreen) { // Update mainScreen clock and date every second
      lv_label_set_text_fmt(mainScreen_clock, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
      lv_label_set_text_fmt(mainScreen_date, "%s, %d %s %d", dowToStr(now.dayOfTheWeek()), now.day(), monthToStr(now.month()), now.year());
    }
    uint16_t tbel = (now.hour() * 100) + now.minute();
    for (int i = 0; i < jw_used->jumlahBel;i++) { // Check bel index every second
      if (tbel == jw_used->jadwalBel[i] && !bellPlayedFlag) { // Ring the audio bell once!

        // Set flag to true to avoid playing audio file more than once
        bellPlayedFlag = true;
        bellPlayedIndex = i;

        ioExpander->write(Expander::I2S_EN, HIGH);
        ioExpander->write(Expander::AUDIO_RELAY, HIGH);
        xSemaphoreTake(audioMutex, portMAX_DELAY);
        // If the audio already playing,
        // Or if the audio is stopped from playing but relay is still on then signal core 0 to play specified audio immediately
        if (isAudioPlaying || stopAudio) {
          if (stopAudio) // Clear stopAudio flag because we play another audio
            stopAudio = false;
          // Signal to core 0 to play the bell immediately
          audioPlayFlag = true;
          log_d("Bell rang! file : %s", audioPath);
        } // If audio is not playing, wait for 2 seconds then play the audio
        else
          preAudioPlay = true;
        strcpy(audioPath, jw_used->belAudioFile[i]);
        xSemaphoreGive(audioMutex);
      }
      if (tbel < jw_used->jadwalBel[i]) {
        nextBelIndex = i;
        break;
      }
      if (i == jw_used->jumlahBel - 1 && tbel >= jw_used->jadwalBel[i]) {
        nextBelIndex = 255;
        break;
      }
    }
    if (bellPlayedFlag) { // Reset the flag if time already past the played jadwalBel
      if (tbel > jw_used->jadwalBel[bellPlayedIndex]) {
        bellPlayedFlag = false;
        log_d("Bell reset!");
      }
    }
    if (jw_used->jumlahBel == 0)
      nextBelIndex = 255;
    if (nextBelIndex != lastNextBelIndex) { // Update next bel
      if (lv_scr_act() == mainScreen) {
        if (nextBelIndex == 255) { // No more bell for today
          lv_label_set_text(mainScreen_nextBellClock, "");
          lv_label_set_text(mainScreen_nextBellName, "Tidak ada bel");
          lv_label_set_text(mainScreen_nextBellAudioFile, "");
        }
        else {
          lv_label_set_text_fmt(mainScreen_nextBellClock, "%02d:%02d", jw_used->jadwalBel[nextBelIndex] / 100, jw_used->jadwalBel[nextBelIndex] % 100);
          lv_label_set_text_fmt(mainScreen_nextBellName, "%s", jw_used->namaBel[nextBelIndex]);
          lv_label_set_text_fmt(mainScreen_nextBellAudioFile, LV_SYMBOL_AUDIO " %s", jw_used->belAudioFile[nextBelIndex]);
        }
      }
      lastNextBelIndex = nextBelIndex;
    }
    if (lastDay != now.day()) {
      jadwalHari_load(&tj_used, jw_used, tj_used.tipeJadwal == TJ_MINGGUAN ? now.dayOfTheWeek() : 0);
      lastDay = now.day();
    }
  }

  lv_task_handler();
  lastSecond = now.second();

  timerDelayStart.IN(preAudioPlay);
  xSemaphoreTake(audioMutex, portMAX_DELAY);
  timerDelayStop.IN(stopAudio);
  xSemaphoreGive(audioMutex);
  if (timerDelayStart.Q()) {
    preAudioPlay = false;
    // Signal to core 0 to play the bell
    xSemaphoreTake(audioMutex, portMAX_DELAY);
    audioPlayFlag = true;
    log_d("Bell rang! file : %s", audioPath);
    xSemaphoreGive(audioMutex);
  }
  if (timerDelayStop.Q()) { // Turn off relay after 2 seconds of signal from core 0 to stop
    stopAudio = false;
    ioExpander->write(Expander::I2S_EN, LOW);
    ioExpander->write(Expander::AUDIO_RELAY, LOW);
  }
  lv_timer_handler(); /* let the GUI do its work */
  delay(1);
}

void audioTask_cb(void* pvParameters) {
  auto is_filename_mp3 = [](const char* filename) -> bool {
    char buffer[256];
    strcpy(buffer, filename);
    for (char* p = buffer; *p; p++) *p = tolower(*p);
    const char* dot = strrchr(buffer, '.');
    if (!dot || dot == buffer) return false;
    return (strcmp(dot + 1, "mp3") == 0);
  };
  log_i("audioTask running on core %d", xPortGetCoreID());

  unsigned long audioCheckMillis = 0;
  bool lastIsAudioPlaying = false;
  for (;;) {
    if (millis() - audioCheckMillis >= 1000) {
      audioCheckMillis = millis();
      xSemaphoreTake(audioMutex, portMAX_DELAY);
      isAudioPlaying = mp3PCM->isRunning();
      if (lastIsAudioPlaying == true && isAudioPlaying == false && audioPlayFlag == false) { // Falling edge detection of isAudioPlaying, and no audioPlay command for core 0
        stopAudio = true;
        log_d("Audio stopped!");
      }
      if (audioPlayFlag) {
        audioPlayFlag = false;
        log_d("Received flag, check %s!", audioPath);
        if (is_filename_mp3(audioPath)) { // Only open the file if it's mp3
          if (mp3PCM->isRunning()) {
            mp3PCM->stop();
            mp3Source->close();
          }
          log_d("Playing %s!", audioPath);
          mp3Source->open(audioPath);
          mp3PCM->begin(mp3Source, i2sOut);
        }
        else
          log_e("File is not mp3!");
      }
      xSemaphoreGive(audioMutex);
      lastIsAudioPlaying = isAudioPlaying;
    }
    if (mp3PCM->isRunning())
    {
      if (!mp3PCM->loop())
      {
        mp3PCM->stop();
        mp3Source->close();
      }
    }
    vTaskDelay(5);
  }
}

void loadMainScreen() {
  now = rtc->now();
  lv_obj_t* label;
  mainScreen = lv_obj_create(NULL);
  lv_obj_add_style(mainScreen, &scr1Bg, 0);
  lv_obj_set_scrollbar_mode(mainScreen, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(mainScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_add_event_cb(mainScreen, [](lv_event_t* event) {
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_TOP) { // Only load mainMenu if correct gesture is applied
      loadMainMenu();
    }
    }, LV_EVENT_GESTURE, NULL);


  lv_obj_add_event_cb(mainScreen, [](lv_event_t* e) {
    if (rtcPowerLostFlag) {
      rtcPowerLostFlag = false;
      modal_create_alert("RTC tidak berjalan!", "Peringatan!", &lv_font_montserrat_20, &lv_font_montserrat_14, bs_white, bs_dark, bs_danger);
    }
    if (rtcBeginFailFlag) {
      rtcBeginFailFlag = false;
      modal_create_alert("RTC tidak terdeteksi!", "Peringatan!", &lv_font_montserrat_20, &lv_font_montserrat_14, bs_white, bs_dark, bs_danger);
    }
    if (sdNotDetectedFlag) {
      sdNotDetectedFlag = false;
      modal_create_alert("Kartu SD tidak terdeteksi!", "Peringatan!", &lv_font_montserrat_20, &lv_font_montserrat_14, bs_white, bs_dark, bs_danger);
    }
    }, LV_EVENT_SCREEN_LOADED, NULL);

  // Jam Utama
  mainScreen_clock = lv_label_create(mainScreen);
  lvc_label_init(mainScreen_clock, &Montserrat_SemiBold91, LV_ALIGN_CENTER, 0, -80, bs_white);
  lv_label_set_text_fmt(mainScreen_clock, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());

  // Tanggal hari ini
  mainScreen_date = lv_label_create(mainScreen);
  lvc_label_init(mainScreen_date, &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, -33, bs_white);
  lv_label_set_text_fmt(mainScreen_date, "%s, %d %s %d", dowToStr(now.dayOfTheWeek()), now.day(), monthToStr(now.month()), now.year());

  // Bel Icon
  label = lv_label_create(mainScreen);
  lvc_label_init(label, &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 0, bs_white);
  lv_label_set_text(label, LV_SYMBOL_BELL);

  // Nama TemplateJadwal used
  mainScreen_tjName = lv_label_create(mainScreen);
  lvc_label_init(mainScreen_tjName, &lv_font_montserrat_12, LV_ALIGN_CENTER, 0, 18, bs_white, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_SCROLL_CIRCULAR, 150);
  lv_label_set_text_fmt(mainScreen_tjName, "Jadwal %s", tj_used.name);

  // Bel Selanjutnya label
  label = lv_label_create(mainScreen);
  lvc_label_init(label, &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 32, bs_white, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_SCROLL_CIRCULAR, 150);
  lv_label_set_text_static(label, "Bel Selanjutnya :");

  // Bel Selanjutnya jam
  mainScreen_nextBellClock = lv_label_create(mainScreen);
  lvc_label_init(mainScreen_nextBellClock, &Montserrat_SemiBold48, LV_ALIGN_CENTER, 0, 58, bs_white);

  // Nama Bel Selanjutnya
  mainScreen_nextBellName = lv_label_create(mainScreen);
  lvc_label_init(mainScreen_nextBellName, &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 86, bs_white, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_SCROLL_CIRCULAR, 150);

  // Bel Audio File
  mainScreen_nextBellAudioFile = lv_label_create(mainScreen);
  lvc_label_init(mainScreen_nextBellAudioFile, &lv_font_montserrat_12, LV_ALIGN_CENTER, 0, 102, bs_white, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_SCROLL_CIRCULAR, 150);

  uint16_t tbel = (now.hour() * 100) + now.minute();
  for (int i = 0; i < jw_used->jumlahBel;i++) {
    log_d("i %d tbel %d jadwalBel[%d] %d\n", i, tbel, i, jw_used->jadwalBel[i]);
    if (tbel < jw_used->jadwalBel[i]) {
      nextBelIndex = i;
      log_d("nextBelIndex reg %d\n", nextBelIndex);
      break;
    }
    if (i == jw_used->jumlahBel - 1 && tbel >= jw_used->jadwalBel[i]) {
      nextBelIndex = 255;
      log_d("nextBelIndex max %d\n", nextBelIndex);
      break;
    }
  }
  if (jw_used->jumlahBel == 0)
    nextBelIndex = 255;
  log_d("nextBelIndex %d\n", nextBelIndex);

  if (nextBelIndex == 255) { // No more bell for today
    lv_label_set_text(mainScreen_nextBellClock, "");
    lv_label_set_text(mainScreen_nextBellName, "Tidak ada bel");
    lv_label_set_text(mainScreen_nextBellAudioFile, "");
  }
  else {
    lv_label_set_text_fmt(mainScreen_nextBellClock, "%02d:%02d", jw_used->jadwalBel[nextBelIndex] / 100, jw_used->jadwalBel[nextBelIndex] % 100);
    lv_label_set_text_fmt(mainScreen_nextBellName, "%s", jw_used->namaBel[nextBelIndex]);
    lv_label_set_text_fmt(mainScreen_nextBellAudioFile, LV_SYMBOL_AUDIO " %s", jw_used->belAudioFile[nextBelIndex]);
  }

  // Swipe ke atas label
  label = lv_label_create(mainScreen);
  lvc_label_init(label, &lv_font_montserrat_12, LV_ALIGN_CENTER, 0, 140, bs_white);
  lv_label_set_text_fmt(label, LV_SYMBOL_UP"\nSwipe ke atas");
  lv_scr_load_anim(mainScreen, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 500, 0, true);
}

void loadMainMenu() {
  lv_obj_t* button, * label;
  mainMenu = lv_obj_create(NULL);

  tabv = lv_tabview_create(mainMenu, LV_DIR_TOP, 50); // Create tabview on top with 50 height

  // Create 4 tab for the tabview
  tab1 = lv_tabview_add_tab(tabv, "Utama");
  tab2 = lv_tabview_add_tab(tabv, "Template\nJadwal");
  tab3 = lv_tabview_add_tab(tabv, "Tambahan");

  lv_obj_t* tab_btns = lv_tabview_get_tab_btns(tabv);
  // Set styles of tab_btns, chosen the most fit
  lv_obj_set_style_bg_color(tab_btns, bs_indigo_700, 0);
  lv_obj_set_style_text_color(tab_btns, bs_white, 0);
  lv_obj_set_style_text_align(tab_btns, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(tab_btns, bs_dark, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(tab_btns, bs_info, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(tab_btns, bs_info, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_pad_left(tab_btns, LV_HOR_RES / 4 - 15, 0);

  // Kembali button on tabview
  button = lv_btn_create(tab_btns);
  // Button position offset is chosen the most fit
  lvc_btn_init(button, LV_SYMBOL_UP"\nKembali", LV_ALIGN_LEFT_MID, -LV_HOR_RES / 4 + 25, 0, &lv_font_montserrat_12);
  lvc_obj_set_pad_wrapper(button, 5, 5); // Set top and bottom pad to 5
  lv_obj_add_event_cb(button, [](lv_event_t* e) {
    loadMainScreen();
    }, LV_EVENT_CLICKED, NULL);

  tabOne();
  tabTwo();
  tabThree();

  lv_scr_load_anim(mainMenu, LV_SCR_LOAD_ANIM_MOVE_TOP, 500, 0, true);
  lv_obj_add_event_cb(mainMenu, [](lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    // Keyboard for text areas
    regularKeyboard = lv_keyboard_create(mainMenu);
    lv_keyboard_set_map(regularKeyboard, LV_KEYBOARD_MODE_TEXT_LOWER, (const char**)regularKeyboard_map, regularKeyboard_controlMap);
    lv_obj_remove_event_cb(regularKeyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(regularKeyboard, kb_custom_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(regularKeyboard, bs_indigo_100, LV_PART_ITEMS);
    lv_obj_add_flag(regularKeyboard, LV_OBJ_FLAG_HIDDEN);

    numericKeyboard = lv_keyboard_create(mainMenu);
    lv_keyboard_set_map(numericKeyboard, LV_KEYBOARD_MODE_TEXT_LOWER, (const char**)numericKeyboard_map, numericKeyboard_controlMap);
    lv_obj_set_style_bg_color(numericKeyboard, bs_indigo_100, LV_PART_ITEMS);
    lv_obj_add_flag(numericKeyboard, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_SCREEN_LOADED, NULL);
}

void tabOne() {
  // Create tab 1 contents
  lv_obj_t* label, * button;
  lv_obj_set_size(tab1, lv_pct(100), lv_pct(100));
  lv_obj_add_style(tab1, &style_flexRow, 0);

  // Create tombol manual box
  // Create container for tombol bel manual
  lv_obj_t* box = lv_obj_create(tab1);
  lv_obj_set_size(box, lv_pct(100), 150);

  // Label for tombol bel manual box
  label = lv_label_create(box);
  lvc_label_init(label, &lv_font_montserrat_24, LV_ALIGN_TOP_LEFT, 0, 4);
  lv_label_set_text_static(label, "Tombol Bel Manual");

  // Create button for atur bel manual
  button = lv_btn_create(box);
  lvc_btn_init(button, "Atur Bel Manual", LV_ALIGN_TOP_RIGHT);

  lv_obj_add_event_cb(button, belManual_btn_cb, LV_EVENT_CLICKED, NULL);

  // Create 4 bel manual button
  for (int i = 0;i < 4;i++) {
    button = lv_btn_create(box);
    lvc_btn_init(button, belManual[i].name, LV_ALIGN_LEFT_MID, i * 110, 30,
      &lv_font_montserrat_12, bs_purple, bs_white,
      LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP, lv_pct(100),
      lv_pct(20), 50);
    lv_obj_set_style_pad_all(button, 5, 0);
    lv_obj_align(lv_obj_get_child(button, 0), LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_height(lv_obj_get_child(button, 0), lv_pct(100));
    lv_obj_add_event_cb(button, [](lv_event_t* e) {
      static WidgetParameterData belManual_wpd;
      static char belManual_message[90] = { 0 };
      lv_obj_t* btn = lv_event_get_target(e);
      lv_obj_t* btnLabel = lv_obj_get_child(btn, 0);
      sprintf(belManual_message, "Apakah anda yakin ingin mengaktifkan bel manual %s?", lv_label_get_text(btnLabel));
      belManual_wpd.issuer = btn;
      modal_create_confirm(&belManual_wpd, belManual_message, "Konfirmasi Bel Manual", &lv_font_montserrat_16, &lv_font_montserrat_14, bs_white, bs_dark, bs_warning);
      }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(button, [](lv_event_t* e) {
      WidgetParameterData* wpd = (WidgetParameterData*)lv_event_get_param(e);

      ioExpander->write(Expander::I2S_EN, HIGH);
      ioExpander->write(Expander::AUDIO_RELAY, HIGH);
      xSemaphoreTake(audioMutex, portMAX_DELAY);
      // If the audio already playing,
      // Or if the audio is stopped from playing but relay is still on then signal core 0 to play specified audio immediately
      if (isAudioPlaying || stopAudio) {
        if (stopAudio) // Clear stopAudio flag because we play another audio
          stopAudio = false;// Signal to core 0 to play the bell immediately
        audioPlayFlag = true;
        log_d("Bell rang! file : %s", audioPath);
      } // If audio is not playing, wait for 2 seconds then play the audio
      else
        preAudioPlay = true;
      strcpy(audioPath, belManual[lv_obj_get_index(wpd->issuer) - 2].audioFile);
      xSemaphoreGive(audioMutex);

      }, LV_EVENT_REFRESH, NULL);
    if (!belManual[i].enabled)
      lv_obj_add_state(button, LV_STATE_DISABLED);
    belManual_btn_pointer[i] = button;
  }

  // End of create tombol manual box

  // Create tabel jadwal box
  // Create another container on tab1 for Tabel Jadwal
  box = lv_obj_create(tab1);
  lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLL_ELASTIC);

  // Create Tabel Jadwal label
  tab1_namaHari = lv_label_create(box);
  lvc_label_init(tab1_namaHari, &lv_font_montserrat_24, LV_ALIGN_TOP_LEFT);
  lv_label_set_text_fmt(tab1_namaHari, "Bel Hari %s", dowToStr(now.dayOfTheWeek()));
  lv_obj_add_event_cb(tab1_namaHari, [](lv_event_t* e) {
    lv_label_set_text_fmt(tab1_namaHari, "Bel Hari %s", dowToStr(now.dayOfTheWeek()));
    }, LV_EVENT_REFRESH, NULL);

  // Create current used template jadwal label
  tab1_namaTj = lv_label_create(box);
  lvc_label_init(tab1_namaTj, &lv_font_montserrat_12, LV_ALIGN_TOP_LEFT, 0, 22);
  lv_label_set_text_fmt(tab1_namaTj, "Template %s", tj_used.name);
  lv_obj_add_event_cb(tab1_namaTj, [](lv_event_t* e) {
    lv_label_set_text_fmt(tab1_namaTj, "Template %s", tj_used.name);
    }, LV_EVENT_REFRESH, NULL);

  // Create Ganti Template Jadwal button
  button = lv_btn_create(box);
  lvc_btn_init(button, "Ganti Template Jadwal", LV_ALIGN_TOP_RIGHT, 0, 0, &lv_font_montserrat_14);
  // Create Ganti Template Jadwal Modal
  lv_obj_add_event_cb(button, tj_ganti_template_btn_cb, LV_EVENT_CLICKED, NULL);

  tabelJadwalHariIni();
}

void tabTwo() {
  lv_obj_t* label, * box, * button;
  // Create another container for Buat Jadwal or tab2
  box = lv_obj_create(tab2);
  lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(box, LV_VER_RES - 50, 0);
  lv_obj_set_style_pad_all(box, 0, 0);

  label = lv_label_create(box);
  lvc_label_init(label, &lv_font_montserrat_24, LV_ALIGN_TOP_LEFT, 13, 22);
  lv_label_set_text_static(label, "List Template Jadwal");

  button = lv_btn_create(box);
  lvc_btn_init(button, "Buat Template\nJadwal Baru", LV_ALIGN_TOP_RIGHT, -13, 13);
  lv_obj_add_event_cb(button, [](lv_event_t* e) {
    if (tj_total_active == 10) {
      modal_create_alert("Tidak dapat menambah template jadwal lagi!", "Peringatan!", &lv_font_montserrat_20, &lv_font_montserrat_14, bs_white, bs_dark, bs_danger);
      return;
    }
    TemplateJadwalBuilder::create(0);
    }, LV_EVENT_CLICKED, NULL);
  tj_table_build();
}

void tabThree() {
  auto dateAndClockModal = [](lv_event_t* e) {
    auto initTextArea = [](lv_obj_t* textArea, int maxLen, lv_coord_t offsetx, lv_coord_t offsety, lv_coord_t width) {
      lv_textarea_set_max_length(textArea, maxLen);
      lv_textarea_set_one_line(textArea, true);
      lv_obj_align(textArea, LV_ALIGN_CENTER, offsetx, offsety);
      lv_obj_set_width(textArea, width);
      lv_obj_set_style_pad_all(textArea, 5, LV_PART_MAIN);
      lv_textarea_set_accepted_chars(textArea, "0123456789");
    };
    auto ta_event_cb = [](lv_event_t* e) {
      lv_obj_t* ta = lv_event_get_target(e);
      lv_event_code_t code = lv_event_get_code(e);
      lv_obj_t* overlay = lv_obj_get_parent(lv_obj_get_parent(ta));
      lv_obj_t* kb = (lv_obj_t*)lv_event_get_user_data(e);
      if (code == LV_EVENT_FOCUSED) {
        lv_obj_set_height(overlay, LV_VER_RES - lv_obj_get_height(kb));
        lv_obj_update_layout(overlay);   /*Be sure the sizes are recalculated*/
        lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(kb);
        lv_obj_update_layout(kb);
      }
      else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_set_height(overlay, LV_VER_RES);
        lv_obj_update_layout(overlay);   /*Be sure the sizes are recalculated*/
        lv_keyboard_set_textarea(kb, NULL);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(kb);
      }
    };
    lv_obj_t* issuer = lv_event_get_target(e);
    int issuerIdx = lv_obj_get_index(issuer);

    lv_obj_t* overlay = lvc_create_overlay();
    lv_obj_remove_event_cb(numericKeyboard, kb_event_cb);
    lv_obj_add_event_cb(numericKeyboard, kb_event_cb, LV_EVENT_ALL, overlay);

    lv_obj_t* modal = lv_obj_create(overlay);
    lv_obj_set_size(modal, 336, 180); // Most fit number
    lv_obj_align(modal, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* modalTitle = lv_label_create(modal);
    lvc_label_init(modalTitle, &lv_font_montserrat_20, LV_ALIGN_TOP_LEFT, 0, -5);
    lv_label_set_text_static(modalTitle, issuerIdx == 0 ? "Atur Jam" : "Atur Tanggal");

    lv_obj_t* componentLabel;

    lv_obj_t* ta = lv_textarea_create(modal);
    initTextArea(ta, 2, -70, 0, 50);
    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_ALL, numericKeyboard);
    componentLabel = lv_label_create(modal);
    lvc_label_init(componentLabel, &lv_font_montserrat_12, LV_ALIGN_CENTER, 0, 0, bs_dark, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP, 50);
    lv_obj_align_to(componentLabel, ta, LV_ALIGN_OUT_TOP_LEFT, 0, 0);
    lv_label_set_text(componentLabel, issuerIdx == 0 ? "Jam" : "Tanggal");

    ta = lv_textarea_create(modal);
    initTextArea(ta, 2, 0, 0, 50);
    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_ALL, numericKeyboard);
    componentLabel = lv_label_create(modal);
    lvc_label_init(componentLabel, &lv_font_montserrat_12, LV_ALIGN_CENTER, 0, 0, bs_dark, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP, 50);
    lv_obj_align_to(componentLabel, ta, LV_ALIGN_OUT_TOP_LEFT, 0, 0);
    lv_label_set_text(componentLabel, issuerIdx == 0 ? "Menit" : "Bulan");

    ta = lv_textarea_create(modal);
    initTextArea(ta, issuerIdx == 0 ? 2 : 4, issuerIdx == 0 ? 70 : 82, 0, issuerIdx == 0 ? 50 : 75);
    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_ALL, numericKeyboard);
    componentLabel = lv_label_create(modal);
    lvc_label_init(componentLabel, &lv_font_montserrat_12, LV_ALIGN_CENTER, 0, 0, bs_dark, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_CLIP, issuerIdx == 0 ? 50 : 75);
    lv_obj_align_to(componentLabel, ta, LV_ALIGN_OUT_TOP_LEFT, 0, 0);
    lv_label_set_text(componentLabel, issuerIdx == 0 ? "Detik" : "Tahun");

    lv_obj_t* modalButton = lv_btn_create(modal);
    lvc_btn_init(modalButton, "Atur", LV_ALIGN_BOTTOM_LEFT, 50, 0, &lv_font_montserrat_12);
    lv_obj_add_event_cb(modalButton, [](lv_event_t* event) {
      lv_obj_t* clickedBtn = lv_event_get_target(event);
      lv_obj_t* overlay = lv_obj_get_parent(lv_obj_get_parent(clickedBtn));
      lv_obj_t* modal = lv_obj_get_parent(clickedBtn);
      lv_obj_t* issuer = (lv_obj_t*)lv_event_get_user_data(event);
      int issuerIdx = lv_obj_get_index(issuer);
      int ta1 = atoi(lv_textarea_get_text(lv_obj_get_child(modal, 1))),
        ta2 = atoi(lv_textarea_get_text(lv_obj_get_child(modal, 3))),
        ta3 = atoi(lv_textarea_get_text(lv_obj_get_child(modal, 5)));
      now = rtc->now();
      if (issuerIdx == 0) {
        rtc->adjust(DateTime(now.year(), now.month(), now.day(), ta1, ta2, ta3));
        log_d("Jam %d:%d:%d", ta1, ta2, ta3);
      }
      else {
        rtc->adjust(DateTime(ta3, ta2, ta1, now.hour(), now.minute(), now.second()));
        log_d("Tanggal %d/%d/%d", ta1, ta2, ta3);
      }
      lv_obj_del(overlay);
      }, LV_EVENT_CLICKED, issuer);

    modalButton = lv_btn_create(modal);
    lvc_btn_init(modalButton, "Batal", LV_ALIGN_BOTTOM_RIGHT, -50, 0, &lv_font_montserrat_12);
    lv_obj_add_event_cb(modalButton, [](lv_event_t* event) {
      void* ovl = lv_event_get_user_data(event);
      lv_obj_del((lv_obj_t*)ovl);
      }, LV_EVENT_CLICKED, overlay);
  };
  auto volumeModal = [](lv_event_t* e) {
    lv_obj_t* overlay = lvc_create_overlay();

    lv_obj_t* modal = lv_obj_create(overlay);
    lv_obj_set_size(modal, 275, 220); // Most fit number
    lv_obj_align(modal, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* modalTitle = lv_label_create(modal);
    lvc_label_init(modalTitle, &lv_font_montserrat_20, LV_ALIGN_TOP_LEFT, 0, -5);
    lv_label_set_text_static(modalTitle, "Atur Volume");

    lv_obj_t* modalRoller = lv_roller_create(modal);
    lv_obj_set_width(modalRoller, lv_pct(100));
    lv_roller_set_visible_row_count(modalRoller, 3);
    lv_obj_set_style_bg_color(modalRoller, bs_indigo_700, LV_PART_SELECTED);
    lv_obj_align(modalRoller, LV_ALIGN_CENTER, 0, -10);
    lv_roller_set_options(modalRoller, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(modalRoller, audioVolume, LV_ANIM_OFF);

    lv_obj_t* modalButton = lv_btn_create(modal);
    lvc_btn_init(modalButton, "Pilih", LV_ALIGN_BOTTOM_LEFT, 50, 0, &lv_font_montserrat_12);
    lv_obj_add_event_cb(modalButton, [](lv_event_t* event) {
      lv_obj_t* clickedBtn = lv_event_get_target(event);
      lv_obj_t* overlay = lv_obj_get_parent(lv_obj_get_parent(clickedBtn));
      lv_obj_t* modalRoller = (lv_obj_t*)lv_event_get_user_data(event);
      int selectedIdx = lv_roller_get_selected(modalRoller);
      audioVolume = selectedIdx;
      volume_store();
      i2sOut->SetGain(volumeToGain(audioVolume));
      lv_obj_del(overlay);
      }, LV_EVENT_CLICKED, modalRoller);

    modalButton = lv_btn_create(modal);
    lvc_btn_init(modalButton, "Batal", LV_ALIGN_BOTTOM_RIGHT, -50, 0, &lv_font_montserrat_12);
    lv_obj_add_event_cb(modalButton, [](lv_event_t* event) {
      void* ovl = lv_event_get_user_data(event);
      lv_obj_del((lv_obj_t*)ovl);
      }, LV_EVENT_CLICKED, overlay);
  };
  lv_obj_t* componentLabel;
  lv_obj_t* box = lv_obj_create(tab3);
  lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(box, LV_VER_RES - 50, 0);
  lv_obj_set_style_pad_all(box, 0, 0);

  lv_obj_t* adjustClockBtn = lv_btn_create(box);
  lvc_btn_init(adjustClockBtn, "Atur Jam", LV_ALIGN_TOP_LEFT, 15, 35);
  lv_obj_add_event_cb(adjustClockBtn, dateAndClockModal, LV_EVENT_CLICKED, NULL);

  lv_obj_t* adjustDateBtn = lv_btn_create(box);
  lvc_btn_init(adjustDateBtn, "Atur Tanggal", LV_ALIGN_TOP_LEFT, 15, 80);
  lv_obj_add_event_cb(adjustDateBtn, dateAndClockModal, LV_EVENT_CLICKED, NULL);

  componentLabel = lv_label_create(box);
  lvc_label_init(componentLabel);
  lv_obj_align_to(componentLabel, adjustClockBtn, LV_ALIGN_OUT_TOP_LEFT, 0, -5);
  lv_label_set_text_static(componentLabel, "Atur Jam dan Tanggal : ");

  lv_obj_t* adjustVolumeBtn = lv_btn_create(box);
  lvc_btn_init(adjustVolumeBtn, "Atur Volume", LV_ALIGN_TOP_LEFT, 15, 145);
  lv_obj_add_event_cb(adjustVolumeBtn, volumeModal, LV_EVENT_CLICKED, NULL);

  componentLabel = lv_label_create(box);
  lvc_label_init(componentLabel);
  lv_obj_align_to(componentLabel, adjustVolumeBtn, LV_ALIGN_OUT_TOP_LEFT, 0, -5);
  lv_label_set_text_static(componentLabel, "Atur Volume");
}

void tabelJadwalHariIni() {
  jadwalHari_load(&tj_used, jw_used, tj_used.tipeJadwal == TJ_MINGGUAN ? now.dayOfTheWeek() : 0);

  static const char* jw_table_header[4] = { "#","Nama","Jam\nBel","File Audio" };
  lv_coord_t col_dsc[] = { 46, 160, 76, 132, LV_GRID_TEMPLATE_LAST };
  // Create the table for current tabel jadwal used for this day
  lv_obj_t* secondBox = lv_obj_get_child(tab1, 1);
  if (lv_obj_get_child(secondBox, 3) != NULL && lv_obj_get_child(secondBox, 3) == jw_lv_list_table) {
    log_d("deleting jw_lv_list_table because it already existed");
    lv_obj_del(jw_lv_list_table);
  }
  jw_lv_list_table = lv_table_create(secondBox);
  lv_obj_add_style(jw_lv_list_table, &style_noBorder, 0);
  lv_obj_set_style_text_align(jw_lv_list_table, LV_TEXT_ALIGN_CENTER, LV_PART_ITEMS | LV_STATE_DEFAULT);
  lvc_obj_set_pad_wrapper(jw_lv_list_table, 255, 255, 2, 2, LV_PART_ITEMS);
  lv_obj_set_style_translate_y(jw_lv_list_table, 50, 0);
  lv_obj_set_height(jw_lv_list_table, LV_SIZE_CONTENT);

  lv_table_set_col_cnt(jw_lv_list_table, 4);
  lv_table_set_row_cnt(jw_lv_list_table, jw_used->jumlahBel + 1);

  for (int i = 0;i < 4;i++) {
    lv_table_set_cell_value(jw_lv_list_table, 0, i, jw_table_header[i]);
    lv_table_set_col_width(jw_lv_list_table, i, col_dsc[i]);
  }
  for (int i = 0; i < jw_used->jumlahBel;i++) {
    lv_table_set_cell_value_fmt(jw_lv_list_table, i + 1, 0, "%d", i + 1);
    lv_table_set_cell_value_fmt(jw_lv_list_table, i + 1, 1, jw_used->namaBel[i]);
    lv_table_set_cell_value_fmt(jw_lv_list_table, i + 1, 2, "%02d:%02d", jw_used->jadwalBel[i] / 100, jw_used->jadwalBel[i] % 100);
    lv_table_set_cell_value_fmt(jw_lv_list_table, i + 1, 3, jw_used->belAudioFile[i]);
  }
  if (jw_used->jumlahBel == 0) {
    lv_table_set_cell_value(jw_lv_list_table, 1, 0, "Tidak ada bel untuk hari ini");
    lv_table_add_cell_ctrl(jw_lv_list_table, 1, 0, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
    lv_table_add_cell_ctrl(jw_lv_list_table, 1, 1, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
    lv_table_add_cell_ctrl(jw_lv_list_table, 1, 2, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
  }
  // End of create tabel jadwal box
}

void tj_table_build() {
  // Total pixel length must be 444px
  static const lv_coord_t tjListWidthDescriptor[] = { 44, 150, 100, 75, 75 };
  static const char* tjListHeader[] = { "#","Nama Template", "Tipe", "Aksi" };
  lv_obj_t* boxTab2 = lv_obj_get_child(tab2, 0);
  if (lv_obj_get_child(boxTab2, 2) != NULL && lv_obj_get_child(boxTab2, 2) == tj_lv_list_table) {
    log_d("deleting tj_lv_list_table because it already existed");
    lv_obj_del(tj_lv_list_table);
  }
  tj_lv_list_table = lv_table_create(boxTab2); // Uses table to save memory
  lv_obj_set_style_translate_y(tj_lv_list_table, 100, 0);
  lv_obj_set_style_pad_all(tj_lv_list_table, 0, LV_PART_MAIN);
  lv_obj_add_style(tj_lv_list_table, &style_noBorder, LV_PART_MAIN);
  lv_obj_set_style_border_side(boxTab2, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_width(boxTab2, 1, LV_PART_MAIN);
  lv_obj_set_height(tj_lv_list_table, LV_SIZE_CONTENT);
  // Set the column and row beforehand to avoid memory reallocation later on
  lv_table_set_col_cnt(tj_lv_list_table, 5);
  lv_table_set_row_cnt(tj_lv_list_table, tj_total_active + 1);

  lv_obj_set_style_pad_top(tj_lv_list_table, 8, LV_PART_ITEMS);
  lv_obj_set_style_pad_bottom(tj_lv_list_table, 8, LV_PART_ITEMS);
  lv_obj_set_style_pad_right(tj_lv_list_table, 0, LV_PART_ITEMS);
  lv_obj_set_style_pad_left(tj_lv_list_table, 0, LV_PART_ITEMS);

  lv_obj_set_style_text_align(tj_lv_list_table, LV_TEXT_ALIGN_CENTER, LV_PART_ITEMS | LV_STATE_DEFAULT);
  lv_table_add_cell_ctrl(tj_lv_list_table, 0, 3, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
  for (int i = 0;i < 5;i++) {
    if (i != 4)
      lv_table_set_cell_value(tj_lv_list_table, 0, i, tjListHeader[i]);
    lv_table_set_col_width(tj_lv_list_table, i, tjListWidthDescriptor[i]);
  }

  for (int i = 0; i < tj_total_active;i++) {
    lv_table_set_cell_value_fmt(tj_lv_list_table, i + 1, 0, "%d", i + 1);
    lv_table_set_cell_value_fmt(tj_lv_list_table, i + 1, 1, "%s", tj_lists[i].name);
    lv_table_set_cell_value_fmt(tj_lv_list_table, i + 1, 2, "%s", tj_lists[i].tipeJadwal == TJ_MINGGUAN ? "Mingguan" : "Harian");
    lv_table_set_cell_value_fmt(tj_lv_list_table, i + 1, 3, "");
    lv_table_set_cell_value_fmt(tj_lv_list_table, i + 1, 4, "");
  }

  if (tj_total_active == 0) {
    lv_table_set_cell_value(tj_lv_list_table, 1, 0, "Tidak ada template jadwal");
    lv_table_add_cell_ctrl(tj_lv_list_table, 1, 0, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
    lv_table_add_cell_ctrl(tj_lv_list_table, 1, 1, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
    lv_table_add_cell_ctrl(tj_lv_list_table, 1, 2, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
  }
  else {
    lv_obj_add_event_cb(tj_lv_list_table, tj_table_draw_cb, LV_EVENT_DRAW_PART_END, NULL); // Callback used to draw the pseudo button on the table (table can't draw object)
    lv_obj_add_event_cb(tj_lv_list_table, tj_table_actionBtn_cb, LV_EVENT_VALUE_CHANGED, NULL); // Callback for action button click
    lv_obj_add_event_cb(tj_lv_list_table, tj_table_refresh_cb, LV_EVENT_REFRESH, NULL); // Callback for action button click
  }
}

void tj_table_refresh_cb(lv_event_t* e) {
  lv_obj_t* table = (lv_obj_t*)lv_event_get_target(e);
  WidgetParameterData* mdc = (WidgetParameterData*)lv_event_get_param(e);
  int row = *(int*)(mdc->param);
  templateJadwal_delete(&tj_lists[row - 1]);
  tj_table_build();
}

void tj_table_actionBtn_cb(lv_event_t* e) {
  lv_obj_t* obj = lv_event_get_target(e);
  uint16_t col;
  uint16_t row;
  lv_table_get_selected_cell(obj, &row, &col);
  if (row == 0)
    return;
  if (col == 3) { // Edit button
    TemplateJadwalBuilder::create(row);
  }
  else if (col == 4) { // Delete button
    tj_issue_row = row;
    tj_modalConfirmData.issuer = obj;
    tj_modalConfirmData.param = &tj_issue_row;
    sprintf(tj_delete_confirm_message, "Apakah anda yakin ingin menghapus template jadwal %s?", lv_table_get_cell_value(obj, row, 1));
    modal_create_confirm(&tj_modalConfirmData, tj_delete_confirm_message, "Hapus Template Jadwal", &lv_font_montserrat_16, &lv_font_montserrat_20, bs_white, bs_dark, bs_danger);
  }
}

void tj_table_draw_cb(lv_event_t* e) {
  lv_obj_t* obj = lv_event_get_target(e);
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(e);
  // id = current row × col count + current column
  uint32_t row = dsc->id / lv_table_get_col_cnt(obj);
  uint32_t col = dsc->id - row * lv_table_get_col_cnt(obj);
  if (dsc->part == LV_PART_ITEMS && col >= 3 && row > 0) { // Draw the button only on column 3 of table and row > 0
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    lv_draw_rect_dsc_init(&rect_dsc);
    label_dsc.font = &lv_font_montserrat_12;
    label_dsc.color = col == 3 ? bs_dark : bs_white;
    label_dsc.align = LV_TEXT_ALIGN_CENTER; // Used for centering text horizontally

    rect_dsc.bg_color = col == 3 ? bs_warning : bs_danger;

    rect_dsc.radius = 6; // Round the rectangular a little bit

    // Draw the rectangular area for button
    lv_area_t sw_area;
    // button length is 100% - 15px padding on left/right side
    sw_area.x1 = dsc->draw_area->x1 + 15;
    sw_area.x2 = dsc->draw_area->x2 - 15;
    // button height is 20px centered from cell height with 10px padding on top side
    int drawHeight = (dsc->draw_area->y2 - dsc->draw_area->y1);
    sw_area.y1 = dsc->draw_area->y1 + (drawHeight / 2) - 10;
    sw_area.y2 = sw_area.y1 + 20;
    label_dsc.ofs_y = ((sw_area.y2 - sw_area.y1) / 2) - 6; // Center text vertically
    lv_draw_rect(dsc->draw_ctx, &rect_dsc, &sw_area); // Draw rectangular for button

    lv_draw_label(dsc->draw_ctx, &label_dsc, &sw_area, col == 3 ? "Edit" : "Hapus", NULL);

  }
}

void tj_ganti_template_btn_cb(lv_event_t* e) {
  auto strcat_c = [](char* str, char c) {
    for (;*str;str++); // note the terminating semicolon here. 
    *str++ = c;
    *str++ = 0;
  };
  char options[TJ_MAX_LEN * FS_MAX_NAME_LEN + 20] = { 0 };
  int selectedIdx = 0;
  lv_obj_t* overlay = lvc_create_overlay();

  lv_obj_t* modal = lv_obj_create(overlay);
  lv_obj_set_size(modal, 275, 220); // Most fit number
  lv_obj_align(modal, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* modalTitle = lv_label_create(modal);
  lvc_label_init(modalTitle, &lv_font_montserrat_20, LV_ALIGN_TOP_LEFT, 0, -5);
  lv_label_set_text_static(modalTitle, "Pilih Template Jadwal");

  for (int i = 0;i < tj_total_active; i++) {
    strcat(options, tj_lists[i].name);
    if (i != tj_total_active - 1) // Don't append newline \n on last index
      strcat_c(options, '\n');
    if (strcmp(tj_lists[i].name, tj_used.name) == 0)
      selectedIdx = i; // Set default selected options to current used TemplateJadwal (tj_used)
  }

  lv_obj_t* modalRoller = lv_roller_create(modal);
  lv_obj_set_width(modalRoller, lv_pct(100));
  lv_roller_set_visible_row_count(modalRoller, 3);
  lv_obj_set_style_bg_color(modalRoller, bs_indigo_700, LV_PART_SELECTED);
  lv_obj_align(modalRoller, LV_ALIGN_CENTER, 0, -10);
  lv_roller_set_options(modalRoller, options, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_selected(modalRoller, selectedIdx, LV_ANIM_OFF);

  lv_obj_t* modalButton = lv_btn_create(modal);
  lvc_btn_init(modalButton, "Pilih", LV_ALIGN_BOTTOM_LEFT, 50, 0, &lv_font_montserrat_12);
  lv_obj_add_event_cb(modalButton, [](lv_event_t* event) {
    lv_obj_t* clickedBtn = lv_event_get_target(event);
    lv_obj_t* overlay = lv_obj_get_parent(lv_obj_get_parent(clickedBtn));
    lv_obj_t* modalRoller = (lv_obj_t*)lv_event_get_user_data(event);
    int selectedIdx = lv_roller_get_selected(modalRoller);

    if (strcmp(tj_lists[selectedIdx].name, tj_used.name) != 0) // Only change tj_used if selected value is not the index of tj_used
      templateJadwal_changeUsedTJ(tj_lists[selectedIdx], true, true);

    lv_obj_del(overlay);
    }, LV_EVENT_CLICKED, modalRoller);

  modalButton = lv_btn_create(modal);
  lvc_btn_init(modalButton, "Batal", LV_ALIGN_BOTTOM_RIGHT, -50, 0, &lv_font_montserrat_12);
  lv_obj_add_event_cb(modalButton, [](lv_event_t* event) {
    void* ovl = lv_event_get_user_data(event);
    lv_obj_del((lv_obj_t*)ovl);
    }, LV_EVENT_CLICKED, overlay);

}

void belManual_btn_cb(lv_event_t* e) {
  lv_obj_t* obj = lv_event_get_target(e); // Atur Bel Manual button object

  lv_obj_t* overlay = lvc_create_overlay(); // Create overlay

  lv_obj_remove_event_cb(regularKeyboard, kb_event_cb);
  lv_obj_add_event_cb(regularKeyboard, kb_event_cb, LV_EVENT_ALL, overlay);

  lv_obj_t* modal = lv_obj_create(overlay); // Create modal
  lv_obj_set_size(modal, lv_pct(100), lv_pct(100)); // Most fit number
  lv_obj_align(modal, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* modalTitle = lv_label_create(modal);
  lvc_label_init(modalTitle, &lv_font_montserrat_20, LV_ALIGN_TOP_LEFT, 0, 7);
  lv_label_set_text_static(modalTitle, "Atur Bel Manual");

  lv_obj_t* modalButton = lv_btn_create(modal); // Modal cancel button
  lvc_btn_init(modalButton, "Batal", LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_add_event_cb(modalButton, [](lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    lv_obj_del(lv_obj_get_parent(lv_obj_get_parent(btn)));
    }, LV_EVENT_CLICKED, NULL);

  modalButton = lv_btn_create(modal); // Modal save button
  lvc_btn_init(modalButton, "Simpan", LV_ALIGN_TOP_RIGHT, -80, 0);
  lv_obj_add_event_cb(modalButton, [](lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    lv_obj_t* modal = lv_obj_get_parent(btn);
    for (int i = 3;i <= 6;i++) { // modalContent is 4th to 7th child of modal
      lv_obj_t* modalContent = lv_obj_get_child(modal, i);
      lv_obj_t* checkbox = lv_obj_get_child(modalContent, 1); // checkbox is 2nd child of modalContent
      lv_obj_t* belName = lv_obj_get_child(modalContent, 3); // belName is 4nd child of modalContent
      lv_obj_t* audioButton = lv_obj_get_child(modalContent, 5); // audioButton is 6nd child of modalContent
      belManual[i - 3].enabled = lv_obj_has_state(checkbox, LV_STATE_CHECKED);
      strcpy(belManual[i - 3].name, lv_textarea_get_text(belName));
      strcpy(belManual[i - 3].audioFile, lv_label_get_text(lv_obj_get_child(audioButton, 0)));

      // No need to set the button text, because it's already pointing to belName[].name char pointer
      // But need to enable/disable button according to checkbox value
      if (!belManual[i - 3].enabled)
        lv_obj_add_state(belManual_btn_pointer[i-3], LV_STATE_DISABLED);
      else
        lv_obj_clear_state(belManual_btn_pointer[i-3], LV_STATE_DISABLED);
    }
    lv_obj_del(lv_obj_get_parent(modal));
    belManual_store(belManual, belManual_len);
    }, LV_EVENT_CLICKED, NULL);

  // Create 4 container for checkbox, name textarea, and file audio choose button for each bel manual instance
  for (int i = 0;i < 4;i++) {
    lv_obj_t* modalContent = lv_obj_create(modal); // Create container for bel manual contents
    lv_obj_set_size(modalContent, lv_pct(100), 55);
    lv_obj_set_style_pad_all(modalContent, 0, 0);
    lv_obj_align(modalContent, LV_ALIGN_TOP_MID, 0, 50 + (60 * i));
    lv_obj_add_style(modalContent, &style_noBorder, 0);
    lv_obj_add_flag(modalContent, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // Number descriptor of the container
    lv_obj_t* label = lv_label_create(modalContent);
    lvc_label_init(label, &lv_font_montserrat_24, LV_ALIGN_LEFT_MID, 10, -1);
    lv_label_set_text_fmt(label, "%d", i + 1);

    // Checkbox of the belmanual
    lv_obj_t* checkbox = lv_checkbox_create(modalContent);
    lv_checkbox_set_text_static(checkbox, "");
    lv_obj_align(checkbox, LV_ALIGN_LEFT_MID, 30, 0);
    lv_obj_set_style_bg_color(checkbox, bs_indigo_500, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(checkbox, bs_indigo_500, LV_PART_INDICATOR);
    if (belManual[i].enabled) // Change the checkbox tick based on belManual value
      lv_obj_add_state(checkbox, LV_STATE_CHECKED);
    lv_obj_update_layout(checkbox); // Make sure the layour is updated
    label = lv_label_create(modalContent); // Label for checkbox
    lv_obj_align_to(label, checkbox, LV_ALIGN_OUT_TOP_LEFT, -5, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_label_set_text_static(label, "Aktif");

    // Textarea for bel name
    lv_obj_t* belName = lv_textarea_create(modalContent);
    lv_textarea_set_max_length(belName, FS_MAX_NAME_LEN - 1); // Maximum input text length of the belName is 32
    lv_textarea_set_one_line(belName, true);
    lv_textarea_set_text(belName, belManual[i].name);
    lv_obj_align(belName, LV_ALIGN_CENTER, -50, 0);
    lv_obj_set_width(belName, lv_pct(40));
    lv_obj_set_style_pad_all(belName, 5, LV_PART_MAIN);
    label = lv_label_create(modalContent); // Label for textarea
    lv_obj_align_to(label, belName, LV_ALIGN_OUT_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_label_set_text_static(label, "Nama Bel");
    lv_obj_add_event_cb(belName, [](lv_event_t* e) { // Text area focus/defocus event (show/hide keyboard)
      lv_event_code_t code = lv_event_get_code(e);
      lv_obj_t* ta = lv_event_get_target(e);
      lv_obj_t* kb = (lv_obj_t*)lv_event_get_user_data(e);
      lv_obj_t* cont = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(ta)));
      if (code == LV_EVENT_FOCUSED) {
        lv_obj_set_height(cont, LV_VER_RES - lv_obj_get_height(kb));
        lv_obj_update_layout(cont);   /*Be sure the sizes are recalculated*/
        lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
      }

      if (code == LV_EVENT_DEFOCUSED) {
        lv_keyboard_set_textarea(kb, NULL);
        lv_obj_set_height(cont, LV_VER_RES);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
      }}, LV_EVENT_ALL, regularKeyboard);

    // Button for choosing audio file
    lv_obj_t* audioButton = lv_btn_create(modalContent);
    lvc_btn_init(audioButton, belManual[i].audioFile, LV_ALIGN_RIGHT_MID, 0, 0, &lv_font_montserrat_14, bs_indigo_500, bs_white, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_SCROLL, lv_pct(100), lv_pct(40));
    lv_obj_set_style_pad_all(audioButton, 6, 0);
    lv_obj_add_event_cb(audioButton, [](lv_event_t* e) { // Open traverser when button clicked
      lv_obj_t* btn = lv_event_get_target(e);
      Traverser::createTraverser(btn, "/"); // The returned value of traverser will be saved on REFRESH event of the issuer
      }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(audioButton, [](lv_event_t* e) { // Store the returned value from traverser to the label of audioButton
      lv_obj_t* label = lv_obj_get_child(lv_event_get_target(e), 0);
      char* traverserValue = (char*)lv_event_get_param(e);
      if (strlen(traverserValue) > 0) // Only store when it's not empty, or traverser is not cancelled
        lv_label_set_text(label, traverserValue); // Need to use non-static, because traverser return char* that might be changed on next traverser call
      }, LV_EVENT_REFRESH, NULL);
    label = lv_label_create(modalContent); // Label for audio file
    lv_obj_align_to(label, audioButton, LV_ALIGN_OUT_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_label_set_text_static(label, "File Audio");
  }
}

lv_obj_t* rollpick_create(WidgetParameterData* wpd, const char* headerTitle, const char* options, const lv_font_t* headerFont, lv_coord_t width, lv_coord_t height) {
  lv_obj_t* overlay = lvc_create_overlay();

  lv_obj_t* modal = lv_obj_create(overlay);
  lv_obj_set_size(modal, width, height); // Most fit number
  lv_obj_align(modal, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* modalTitle = lv_label_create(modal);
  lvc_label_init(modalTitle, headerFont, LV_ALIGN_TOP_LEFT, 0, -5);
  lv_label_set_text_static(modalTitle, headerTitle);

  lv_obj_t* modalRoller = lv_roller_create(modal);
  lv_roller_set_options(modalRoller, options, LV_ROLLER_MODE_NORMAL);
  lv_obj_set_width(modalRoller, lv_pct(100));
  lv_roller_set_visible_row_count(modalRoller, 3);
  lv_obj_set_style_bg_color(modalRoller, bs_indigo_700, LV_PART_SELECTED);
  lv_obj_align(modalRoller, LV_ALIGN_CENTER, 0, -10);

  lv_obj_t* modalButton = lv_btn_create(modal);
  lvc_btn_init(modalButton, "Pilih", LV_ALIGN_BOTTOM_LEFT, 50, 0, &lv_font_montserrat_12);
  lv_obj_add_event_cb(modalButton, [](lv_event_t* event) {
    lv_obj_t* clickedBtn = lv_event_get_target(event);
    lv_obj_t* modal = lv_obj_get_parent(clickedBtn);
    lv_obj_t* overlay = lv_obj_get_parent(modal);
    lv_obj_t* modalRoller = lv_obj_get_child(modal, 1);
    WidgetParameterData* wpd = (WidgetParameterData*)lv_event_get_user_data(event);
    *(uint16_t*)wpd->param = lv_roller_get_selected(modalRoller);
    // Get selected roller value
    lv_event_send(wpd->issuer, LV_EVENT_REFRESH, wpd);
    // Exit from modal
    lv_obj_del(overlay);
    }, LV_EVENT_CLICKED, wpd);

  modalButton = lv_btn_create(modal);
  lvc_btn_init(modalButton, "Batal", LV_ALIGN_BOTTOM_RIGHT, -50, 0, &lv_font_montserrat_12);
  lv_obj_add_event_cb(modalButton, [](lv_event_t* event) {
    void* ovl = lv_event_get_user_data(event);
    lv_obj_del((lv_obj_t*)ovl);
    }, LV_EVENT_CLICKED, overlay);
  return modalRoller;
}

// Function to add copy/paste capability on keyboard on top of default keyboard callback
void kb_custom_event_cb(lv_event_t* e) {
  lv_obj_t* obj = lv_event_get_target(e);
  lv_keyboard_t* kb = (lv_keyboard_t*)obj;
  uint16_t btn_id = lv_btnmatrix_get_selected_btn(obj);
  if (btn_id == LV_BTNMATRIX_BTN_NONE) return;
  const char* txt = lv_btnmatrix_get_btn_text(obj, lv_btnmatrix_get_selected_btn(obj));
  if (kb->ta == NULL) return;
  if (strcmp(txt, "Copy") == 0)
    strcpy(kb_copy_buffer, lv_textarea_get_text(kb->ta));
  else if (strcmp(txt, "Paste") == 0)
    lv_textarea_add_text(kb->ta, kb_copy_buffer);
  else if (strcmp(txt, "Paste") == 0)
    lv_textarea_add_text(kb->ta, kb_copy_buffer);
  else if (strcmp(txt, LV_SYMBOL_NEW_LINE) == 0) { // Pressing enter will send LV_EVENT_READY
    lv_res_t res = lv_event_send(obj, LV_EVENT_READY, NULL);
    if (res != LV_RES_OK) return;
  }
  else if (strcmp(txt, "abc") == 0) {
    kb->mode = LV_KEYBOARD_MODE_TEXT_LOWER;
    lv_btnmatrix_set_map(obj, (const char**)regularKeyboard_map);
    lv_btnmatrix_set_ctrl_map(obj, regularKeyboard_controlMap);
    return;
  }
  else
    lv_keyboard_def_event_cb(e);
}

void kb_event_cb(lv_event_t* e) {
  lv_obj_t* kb = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t* cont = (lv_obj_t*)lv_event_get_user_data(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_obj_set_height(cont, LV_VER_RES);
    lv_keyboard_set_textarea(kb, NULL);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  }
}

lv_obj_t* modal_create_confirm(WidgetParameterData* modalConfirmData, const char* message, const char* headerText, const lv_font_t* headerFont, const lv_font_t* messageFont, lv_color_t headerTextColor, lv_color_t textColor, lv_color_t headerColor, const char* confirmButtonText, const char* cancelButtonText, lv_coord_t xSize, lv_coord_t ySize) {
  lv_obj_t* modal = modal_create_alert(message, headerText, headerFont, messageFont, headerTextColor, textColor, headerColor, cancelButtonText, xSize, ySize);
  lv_obj_t* okButton = lv_btn_create(modal);
  lvc_btn_init(okButton, confirmButtonText, LV_ALIGN_BOTTOM_RIGHT, -120, -15);
  lv_obj_add_event_cb(okButton, [](lv_event_t* e) {
    WidgetParameterData* modalConfirmData = (WidgetParameterData*)lv_event_get_user_data(e);
    lv_obj_t* btn = lv_event_get_target(e);
    lv_event_send(modalConfirmData->issuer, LV_EVENT_REFRESH, modalConfirmData);
    lv_obj_del(lv_obj_get_parent(lv_obj_get_parent(btn)));
    }, LV_EVENT_CLICKED, modalConfirmData);
  return modal;
}

lv_obj_t* modal_create_alert(const char* message, const char* headerText, const lv_font_t* headerFont, const lv_font_t* messageFont, lv_color_t headerTextColor, lv_color_t textColor, lv_color_t headerColor, const char* buttonText, lv_coord_t xSize, lv_coord_t ySize) {
  lv_obj_t* overlay = lvc_create_overlay();

  lv_obj_t* modal = lv_obj_create(overlay);
  lv_obj_center(modal);
  lv_obj_set_size(modal, xSize, ySize);
  lv_obj_set_style_pad_all(modal, 0, 0);

  lv_obj_t* modalHeader = lv_obj_create(modal);
  lv_obj_align(modalHeader, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_size(modalHeader, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_add_style(modalHeader, &style_zeroRadius, 0);
  lv_obj_set_style_bg_color(modalHeader, headerColor, 0);

  lv_obj_t* warningLabel = lv_label_create(modalHeader);
  lvc_label_init(warningLabel, &lv_font_montserrat_20, LV_ALIGN_TOP_LEFT, 0, 0, headerTextColor, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_WRAP, lv_pct(100));
  lv_label_set_text_static(warningLabel, headerText);

  lv_obj_t* error = lv_label_create(modal);
  lvc_label_init(error, &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 0, textColor, LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_WRAP, lv_pct(100));
  lv_label_set_text_static(error, message);

  lv_obj_t* okButton = lv_btn_create(modal);
  lvc_btn_init(okButton, buttonText, LV_ALIGN_BOTTOM_RIGHT, -15, -15);
  lv_obj_add_event_cb(okButton, [](lv_event_t* e) {lv_obj_del((lv_obj_t*)lv_event_get_user_data(e));}, LV_EVENT_CLICKED, overlay);
  return modal;
}

bool belManual_load(BelManual* bm_target, size_t len) {
  if (!sdBeginFlag)
    return false;
  File file = ESPSYS_FS.open(PATH_ESPSYS"belManual.bin", "r");
  if (!file) {
    file.close();
    log_e("Error opening file!");
    return false;
  }
  file.readBytes((char*)bm_target, sizeof(BelManual) * len);
  file.close();
  return true;
}
bool belManual_store(BelManual* bm_target, size_t len) {
  if (!sdBeginFlag)
    return false;
  File file = ESPSYS_FS.open(PATH_ESPSYS"belManual.bin", "w");
  if (!file) {
    file.close();
    log_e("Error opening file!");
    return false;
  }
  file.write((const uint8_t*)bm_target, sizeof(BelManual) * len);
  file.close();
  return true;
}
bool jadwalHari_load(TemplateJadwal* tj_target, JadwalHari* jwh_target, int num) {
  if (!sdBeginFlag)
    return false;
  if (strlen(tj_target->name) == 0) {
    log_e("tj_target->name mustn't empty!%s");
    return false;
  }
  char tempPath[128];
  sprintf(tempPath, PATH_TJ"%s/%d", tj_target->name, num);
  log_d("load jh %s", tempPath);
  File file = ESPSYS_FS.open(tempPath, "r");
  if (!file) {
    file.close();
    log_e("Error opening file!");
    return false;
  }
  file.readBytes((char*)jwh_target, sizeof(JadwalHari));
  file.close();
  return true;
}
bool jadwalHari_store(TemplateJadwal* tj_target, JadwalHari* jwh_target, int num) {
  if (!sdBeginFlag)
    return false;
  char tempPath[128];
  sprintf(tempPath, PATH_TJ"%s/%d", tj_target->name, num);
  log_d("store jh %s", tempPath);
  File file = ESPSYS_FS.open(tempPath, "w+");
  if (!file) {
    file.close();
    log_e("Error opening file!");
    return false;
  }
  file.write((const uint8_t*)jwh_target, sizeof(JadwalHari));
  file.close();
  return true;
}
bool templateJadwal_load(TemplateJadwal* tj_target, const char* path) {
  if (!sdBeginFlag)
    return false;
  File file = ESPSYS_FS.open(path, "r");
  log_d("load tj at %s", path);
  if (!file) {
    file.close();
    log_e("Error opening file!");
    return false;
  }
  file.readBytes((char*)tj_target, sizeof(TemplateJadwal));
  file.close();
  log_d("loaded %s type : %s", tj_target->name, tj_target->tipeJadwal == TJ_HARIAN ? "harian" : "mingguan");
  return true;
}
bool templateJadwal_store(TemplateJadwal* tj_target) {
  if (!sdBeginFlag)
    return false;
  char path[64];
  sprintf(path, PATH_TJ"%s.bin", tj_target->name);
  log_d("store tj at %s", path);
  File file = ESPSYS_FS.open(path, "w+");
  if (!file) {
    file.close();
    log_e("Error opening file!");
    return false;
  }
  file.write((const uint8_t*)tj_target, sizeof(TemplateJadwal));
  file.close();
  return true;
}
bool templateJadwal_activeName_update(const char* activeName) {
  if (!sdBeginFlag)
    return false;
  char temp[33];
  strcpy(temp, activeName);
  File file = ESPSYS_FS.open(PATH_ESPSYS"tj_active_name.bin", "w+");
  log_d("updating tj_active_name.bin to %s", activeName);
  if (!file) {
    file.close();
    log_e("Error opening file!");
    return false;
  }
  file.write((const uint8_t*)temp, sizeof(TemplateJadwal::name));
  file.close();
  log_d("updating tj_active_name.bin OK\n");
  return true;
}
bool templateJadwal_activeName_load() {
  if (!sdBeginFlag)
    return false;
  File file = ESPSYS_FS.open(PATH_ESPSYS"tj_active_name.bin", "r");
  log_d("loading tj_active_name.bin");
  if (!file) {
    file.close();
    log_e("Error opening file!");
    return false;
  }
  file.readBytes((char*)tj_active_name, sizeof(TemplateJadwal::name));
  file.close();
  log_d("tj_active_name.bin loaded : %s", tj_active_name);
  return true;
}
bool templateJadwal_list_load() {
  if (!sdBeginFlag)
    return false;
  int tjIndex = 0;
  bool tjUsedFound = false;
  log_d("Loading TemplateJadwal lists....");
  File root = ESPSYS_FS.open(PATH_ESPSYS"tj");
  if (!root) {
    log_e("Error : Root doesn't exist!");
    return false;
  }
  if (!root.isDirectory()) {
    log_e("Error : Root is not directory!");
    return false;
  }
  File file = root.openNextFile();
  while (file)
  {
    if (!file.isDirectory()) {
      templateJadwal_load(&tj_lists[tjIndex], file.path());
      log_d("compare loaded %s to tj_active_name %s", tj_lists[tjIndex].name, tj_active_name);
      if (strcmp(tj_lists[tjIndex].name, tj_active_name) == 0) {
        templateJadwal_changeUsedTJ(tj_lists[tjIndex], false, false);
        tjUsedFound = true;
        log_d("tj_used name %s type %d", tj_used.name, tj_used.tipeJadwal);
      }
      tjIndex++;
    }
    file = root.openNextFile();
  }
  file.close();
  root.close();
  tj_total_active = tjIndex;
  if (!tjUsedFound) {
    log_d("Can't find active tj");
    templateJadwal_changeUsedTJ(tj_lists[tjIndex - 1], false, false);
    log_d("tj_active set to %s %d", tj_used.name, tj_used.tipeJadwal);
    templateJadwal_activeName_update(tj_used.name);
  }
  return true;
}
bool templateJadwal_create(TemplateJadwal* tj_target) {
  if (!sdBeginFlag)
    return false;
  log_d("\nTemplateJadwal Create Dummy");
  char path[64];
  sprintf(path, PATH_TJ"%s.bin", tj_target->name);
  log_d("Create binary at %s", path);
  File file = ESPSYS_FS.open(path, "w+");
  if (!file) {
    file.close();
    log_e("Error opening file!");
    return false;
  }
  file.write((uint8_t*)tj_target, sizeof(TemplateJadwal));
  file.close();
  sprintf(path, PATH_TJ"%s", tj_target->name);
  log_d("Create dir at %s", path);
  if (ESPSYS_FS.mkdir(path) == -1) {
    log_e("Error : %s\n", strerror(errno));
    return false;
  }
  for (int i = 0; i < 7; i++) {
    sprintf(path, PATH_TJ"%s/%d", tj_target->name, i);
    log_d("Create JW binary at %s", path);
    file = ESPSYS_FS.open(path, "w+");
    if (!file) {
      file.close();
      log_e("Error opening file!");
      return false;
    }
    file.write((uint8_t*)&jw_empty, sizeof(JadwalHari));
    file.close();
  }
  log_d("OK\n\n");
  return true;
}
bool templateJadwal_changeUsedTJ(TemplateJadwal to, bool refreshElements, bool updateBinary) {
  if (!sdBeginFlag)
    return false;
  tj_used = to;
  if (refreshElements) {
    lv_obj_t* scrAct = lv_scr_act();
    if (scrAct == mainMenu && mainMenu != mainScreen) { // Updates objects on screen 2
      lv_event_send(tab1_namaTj, LV_EVENT_REFRESH, NULL);
    }
    tabelJadwalHariIni(); // Update tabelJadwalHariIni because template jadwal is changed
  }
  if (updateBinary)
    return templateJadwal_activeName_update(tj_used.name);
  return true;
}
bool templateJadwal_delete(TemplateJadwal* tj_target) {
  if (!sdBeginFlag)
    return false;
  log_d("\nTemplateJadwal Delete");
  char path[64];
  sprintf(path, PATH_TJ"%s", tj_target->name);
  log_d("Delete binary folder at %s", path);
  if (!ESPSYS_FS.rmdir(path)) {
    log_e("Error removing folder!");
    return false;
  }
  sprintf(path, PATH_TJ"%s.bin", tj_target->name);
  log_d("Delete binary at %s", path);
  if (!ESPSYS_FS.remove(path)) {
    log_e("Error : %s", strerror(errno));
    return false;
  }
  log_d("OK\n");
  templateJadwal_list_load(); // Reload the lists after success delete
  return true;
}
bool volume_store() {
  if (!sdBeginFlag)
    return false;
  File file = ESPSYS_FS.open(PATH_ESPSYS"volume.bin", "w+");
  log_d("updating volume.bin");
  if (!file) {
    file.close();
    log_e("Error opening file!");
    return false;
  }
  file.write((uint8_t*)&audioVolume, sizeof(audioVolume));
  file.close();
  log_d("volume.bin updated");
  return true;
}
bool volume_load() {
  if (!sdBeginFlag)
    return false;
  File file = ESPSYS_FS.open(PATH_ESPSYS"volume.bin", "r");
  log_d("loading volume.bin");
  if (!file) {
    file.close();
    log_e("Error opening file!");
    return false;
  }
  file.readBytes((char*)&audioVolume, sizeof(audioVolume));
  file.close();
  log_d("volume.bin loaded : %d", audioVolume);
  return true;
}
// Traverser functions declaration
void Traverser::createTraverser(lv_obj_t* issuer, const char* dir, bool build) {
  strcpy(traverseDirBuffer, dir); // Store target traverse directory to buffer
  traverserIssuer = issuer; // Save the issuer object pointer
  if (build) { // Build can't set to false when called first on issuer
    exist = true;
    overlay = lvc_create_overlay();

    modal = lv_obj_create(overlay);
    lv_obj_set_size(modal, lv_pct(100), lv_pct(100)); // Most fit number
    lv_obj_align(modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    modalTitle = lv_label_create(modal);
    lvc_label_init(modalTitle, &lv_font_montserrat_20, LV_ALIGN_TOP_LEFT, 13, 13);
    lv_label_set_text_static(modalTitle, "Pilih File");

    traverseBackButton = lv_btn_create(modal);
    lvc_btn_init(traverseBackButton, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, 15, 47);
    lvc_obj_set_pad_wrapper(traverseBackButton, 0, 0, 0, 3);
    lv_obj_set_size(traverseBackButton, 24, 24);
    lv_obj_set_style_radius(traverseBackButton, 12, 0);
    lv_obj_add_event_cb(traverseBackButton, traverseBack, LV_EVENT_CLICKED, NULL);

    traversePathLabel = lv_label_create(modal);
    lvc_label_init(traversePathLabel, &lv_font_montserrat_16, LV_ALIGN_TOP_LEFT, 43, 50);
    lv_label_set_text_static(traversePathLabel, LV_SYMBOL_SD_CARD);
    traversePathLabel = lv_label_create(modal);
    lvc_label_init(traversePathLabel, &lv_font_montserrat_16, LV_ALIGN_TOP_LEFT, 61, 50, bs_dark, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_SCROLL_CIRCULAR, lv_pct(85));
    lv_label_set_text_static(traversePathLabel, "/");

    traverseCancelButton = lv_btn_create(modal);
    lvc_btn_init(traverseCancelButton, "Batal", LV_ALIGN_TOP_RIGHT, -13, 7, &lv_font_montserrat_12);
    lv_obj_add_event_cb(traverseCancelButton, [](lv_event_t* event) { // Exit from modal passing empty string to the issuer
      exist = false;
      strcpy(traverserReturnParam, "");
      lv_event_send(traverserIssuer, LV_EVENT_REFRESH, traverserReturnParam);
      lv_obj_del(overlay);
      }, LV_EVENT_CLICKED, overlay);
  }

  traverseBox = lv_obj_create(modal); // Create new traverseBox

  lv_label_set_text_static(traversePathLabel, traverseDirBuffer); // Store/update new traverse directory into the label

  // Enable/disable back button based on current traversed dir
  if (strlen(traverseDirBuffer) > 1)  // traversed dir is not root (length is more than 1)
    lv_obj_clear_state(traverseBackButton, LV_STATE_DISABLED);
  else
    lv_obj_add_state(traverseBackButton, LV_STATE_DISABLED);

  // Calculate total file and folder within path
  int rowLen = 0;

  File root = SD.open(traverseDirBuffer);
  File file = root.openNextFile();
  while (file)
  {
    if (strcmp(file.name(), "System Volume Information") != 0 && strcmp(file.name(), "espsys") != 0)
      rowLen++;
    file = root.openNextFile();
  }
  root.close();
  file.close();

  lv_obj_set_size(traverseBox, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(traverseBox, 0, 0);
  lv_obj_add_style(traverseBox, &style_noBorder, 0);
  lv_obj_set_style_translate_y(traverseBox, 75, 0);
  lv_obj_set_scrollbar_mode(traverseBox, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(traverseBox, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* table = lv_table_create(traverseBox); // Uses table to save memory
  lv_obj_set_style_pad_all(table, 0, LV_PART_MAIN);
  lv_obj_add_style(table, &style_noBorder, LV_PART_MAIN);
  // Set the column and row beforehand to avoid memory reallocation later on
  lv_table_set_col_cnt(table, 4);
  lv_table_set_row_cnt(table, rowLen + 1);

  lv_obj_set_style_pad_top(table, 8, LV_PART_ITEMS);
  lv_obj_set_style_pad_bottom(table, 8, LV_PART_ITEMS);
  lv_obj_set_style_pad_right(table, 0, LV_PART_ITEMS);
  lv_obj_set_style_pad_left(table, 0, LV_PART_ITEMS);

  lv_obj_set_style_text_align(table, LV_TEXT_ALIGN_CENTER, LV_PART_ITEMS | LV_STATE_DEFAULT);

  for (int i = 0;i < 4;i++) {
    lv_table_set_cell_value(table, 0, i, traverserTableHeader[i]);
    lv_table_set_col_width(table, i, traverserCol_widthDescriptor[i]);
  }

  rowLen = 0;

  root = SD.open(traverseDirBuffer);
  file = root.openNextFile();
  while (file)
  {
    if (strcmp(file.name(), "System Volume Information") != 0 && strcmp(file.name(), "espsys") != 0) {
      rowLen++;
      bool isDir = file.isDirectory();
      unsigned long size = (unsigned long)file.size();
      lv_table_set_cell_value_fmt(table, rowLen, 0, "%d", rowLen);
      lv_table_set_cell_value_fmt(table, rowLen, 1, (isDir) ? LV_SYMBOL_DIRECTORY " %s" : LV_SYMBOL_FILE " %s", file.name());
      if (isDir)
        lv_table_set_cell_value_fmt(table, rowLen, 2, " ");
      else {
        char sizestr[16] = { 0 };
        sprintf(sizestr, "%.1f %s", size < 1024 ? float(size) : size < 1048576 ? float(size) / 1024. : float(size) / 1048576., size < 1024 ? "B" : size < 1048576 ? "KB" : "MB");
        lv_table_set_cell_value_fmt(table, rowLen, 2, sizestr); // Somehow the table build-in format can't take float, so we buffer it with another string
      }
      lv_table_set_cell_value(table, rowLen, 3, "");
    }
    file = root.openNextFile();
  }
  root.close();
  file.close();

  lv_obj_add_event_cb(table, traverserTableDrawEventCallback, LV_EVENT_DRAW_PART_END, NULL); // Callback used to draw the pseudo button on the table (table can't draw object)
  lv_obj_add_event_cb(table, traverserActionButtonClicked, LV_EVENT_VALUE_CHANGED, NULL); // Callback for traverseActionButton click
}
void Traverser::traverseBack(lv_event_t* event) {
  if (!exist)
    return;
  char buffer[TRAVERSER_MAX_TRAVERSING_LEN] = { 0 };
  char* pLastSlash = strrchr(traverseDirBuffer, '/');
  int lastSlashIdx = pLastSlash - traverseDirBuffer;
  lastSlashIdx = lastSlashIdx < TRAVERSER_MAX_TRAVERSING_LEN ? lastSlashIdx : TRAVERSER_MAX_TRAVERSING_LEN; // Get the index of the last slash "/"
  log_d("traverseDirBuffer %s\nlastSlashidx %d", traverseDirBuffer, lastSlashIdx);
  strncpy(buffer, traverseDirBuffer, lastSlashIdx == 0 ? 1 : lastSlashIdx); // Copy just until the index of the last slash "/" to chop the last directory
  log_d("buffer %s", buffer);
  // Copy the chopped back one directory string to traverseDirBuffer
  strcpy(traverseDirBuffer, buffer);
  log_d("traversedBack %s", traverseDirBuffer);

  lv_label_set_text_static(traversePathLabel, traverseDirBuffer); // Store/update new traversePath into the label

  // Rebuild traverser with new backed up directory
  lv_obj_del(traverseBox);
  createTraverser(traverserIssuer, traverseDirBuffer, false);
}
void Traverser::traverserActionButtonClicked(lv_event_t* e) {
  if (!exist)
    return;
  lv_obj_t* obj = lv_event_get_target(e);
  uint16_t col;
  uint16_t row;
  lv_table_get_selected_cell(obj, &row, &col);
  if (col == 3 && row > 0) { // Action button is on this row and col
    bool isDir = strncmp(lv_table_get_cell_value(obj, row, 1), "\xEF\x81\xBB", 3) == 0;
    const char* nameChopped = lv_table_get_cell_value(obj, row, 1) + 4; // Skip first four char of dir/file name because, the first 4 char is symbol + space

    if (isDir) { // Traverse directory
      if (strlen(traverseDirBuffer) > 1) // Append "/" to new traverseDir when traversePath is not root
        strncat(traverseDirBuffer, "/", 1);
      strcat(traverseDirBuffer, nameChopped); // Concat clicked directory name to current traverseDirBuffer

      lv_obj_del(traverseBox);
      if (strlen(traverseDirBuffer) < 100)
        createTraverser(traverserIssuer, traverseDirBuffer, false);
      else {
        createTraverser(traverserIssuer, "/", false); // Can't go even deeper, go to root
        // And show warning modal
        modal_create_alert("Tidak dapat menjelajah lebih dalam lagi");
      }
    }
    else { // Choose file
      strcpy(traverserReturnParam, traverseDirBuffer); // Copy 
      if (strlen(traverseDirBuffer) > 1) // Append "/" to traverseReturnParam when current traversed path is not root (avoid something like this /somedirfile.mp3)
        strncat(traverserReturnParam, "/", 1);
      strcat(traverserReturnParam, nameChopped); // Concat name to the path
      exist = false;
      lv_event_send(traverserIssuer, LV_EVENT_REFRESH, traverserReturnParam); // Return the file path for the audio file
      lv_obj_del(overlay); // Delete traverser
    }
  }
}
void Traverser::traverserTableDrawEventCallback(lv_event_t* e) {
  if (!exist)
    return;
  lv_obj_t* obj = lv_event_get_target(e);
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(e);
  // id = current row × col count + current column
  uint32_t row = dsc->id / lv_table_get_col_cnt(obj);
  uint32_t col = dsc->id - row * lv_table_get_col_cnt(obj);
  if (dsc->part == LV_PART_ITEMS && col == 3 && row > 0) { // Draw the button only on column 3 of table and row > 0
    bool isDir = strncmp(lv_table_get_cell_value(obj, row, 1), "\xEF\x81\xBB", 3) == 0;
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    lv_draw_rect_dsc_init(&rect_dsc);
    label_dsc.font = &lv_font_montserrat_12;
    label_dsc.color = bs_white;
    label_dsc.align = LV_TEXT_ALIGN_CENTER; // Used for centering text horizontally
    rect_dsc.bg_color = bs_indigo_500;
    rect_dsc.radius = 6; // Round the rectangular a little bit

    // Draw the rectangular area for button
    lv_area_t sw_area;
    // button length is 100% - 15px padding on left/right side
    sw_area.x1 = dsc->draw_area->x1 + 15;
    sw_area.x2 = dsc->draw_area->x2 - 15;
    // button height is 20px centered from cell height with 10px padding on top side
    int drawHeight = (dsc->draw_area->y2 - dsc->draw_area->y1);
    sw_area.y1 = dsc->draw_area->y1 + (drawHeight / 2) - 10;
    sw_area.y2 = sw_area.y1 + 20;
    label_dsc.ofs_y = ((sw_area.y2 - sw_area.y1) / 2) - 6; // Center text vertically
    lv_draw_rect(dsc->draw_ctx, &rect_dsc, &sw_area); // Draw rectangular for button
    lv_draw_label(dsc->draw_ctx, &label_dsc, &sw_area, isDir ? traverserActionButtonLabel[1] : traverserActionButtonLabel[0], NULL);
  }
}

void TemplateJadwalBuilder::create(int row) {
  changed = false;
  belChanged = false;
  createNew = (row == 0);
  if (createNew) {
    templateJadwal_create((TemplateJadwal*)&tj_empty);
    templateJadwal_load(&tj_temp, PATH_TJ"new.bin");
    changed = true;
  }
  tj_target = createNew ? &tj_temp : &tj_lists[row - 1];
  strcpy(tj_oldName, createNew ? "new" : lv_table_get_cell_value(tj_lv_list_table, row, 1));

  log_d("loaded %s %d/%s", tj_target->name, tj_target->tipeJadwal, tj_target->tipeJadwal == TJ_MINGGUAN ? "Mingguan" : "Harian");

  btj_overlay = lvc_create_overlay();

  lv_obj_remove_event_cb(regularKeyboard, kb_event_cb);
  lv_obj_add_event_cb(regularKeyboard, kb_event_cb, LV_EVENT_ALL, btj_overlay);

  btj_modal = lv_obj_create(btj_overlay);
  lv_obj_set_size(btj_modal, lv_pct(100), lv_pct(100));
  lv_obj_set_style_pad_all(btj_modal, 0, 0);

  btj_modal_header = lv_label_create(btj_modal);
  lvc_label_init(btj_modal_header, &lv_font_montserrat_20, LV_ALIGN_TOP_LEFT, 15, 15);
  if (createNew)
    lv_label_set_text_static(btj_modal_header, "Buat Template\n Jadwal Baru");
  else
    lv_label_set_text_fmt(btj_modal_header, "Edit %s", tj_oldName);

  btj_modal_cancelBtn = lv_btn_create(btj_modal);
  lvc_btn_init(btj_modal_cancelBtn, "Keluar", LV_ALIGN_TOP_RIGHT, -15, 15);
  lv_obj_add_event_cb(btj_modal_cancelBtn, [](lv_event_t* e) {
    if (changed) {
      btj_wpd.issuer = btj_modal_cancelBtn;
      modal_create_confirm(&btj_wpd, "Terdapat perubahan dalam template jadwal, Apakah anda yakin untuk keluar?\nKlik \"OK\" untuk membuang perubahan.\nKlik tombol \"Simpan\" apabila ingin menyimpan perubahan dalam template jadwal", "Peringatan", &lv_font_montserrat_16, &lv_font_montserrat_20, bs_dark, bs_dark, bs_warning, "Ok", "Batal", lv_pct(80), lv_pct(90));
      return;
    }
    if (belChanged) {
      btj_wpd.issuer = btj_modal_cancelBtn;
      modal_create_confirm(&btj_wpd, "Terdapat perubahan dalam daftar bel, Apakah anda yakin untuk keluar?\nKlik \"OK\" untuk membuang perubahan.\nKlik tombol \"Simpan bel\" apabila ingin menyimpan perubahan dalam template jadwal", "Peringatan", &lv_font_montserrat_16, &lv_font_montserrat_20, bs_dark, bs_dark, bs_warning, "Ok", "Batal", lv_pct(80), lv_pct(90));
      return;
    }
    lv_obj_t* overlay = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
    lv_obj_del(overlay);
    }, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(btj_modal_cancelBtn, [](lv_event_t* e) {
    lv_obj_t* overlay = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
    if (createNew && changed) {
      templateJadwal_delete(tj_target);
    }
    lv_obj_del(overlay);
    }, LV_EVENT_REFRESH, NULL);

  btj_modal_saveBtn = lv_btn_create(btj_modal);
  lvc_btn_init(btj_modal_saveBtn, "Simpan", LV_ALIGN_TOP_RIGHT, -100, 15);
  lv_obj_add_event_cb(btj_modal_saveBtn, [](lv_event_t* e) {
    log_d("Saving TJ");
    if (strlen(lv_textarea_get_text(btj_modal_tjNameTextArea)) == 0) {
      modal_create_alert("Nama tidak boleh kosong!", "Gagal!", &lv_font_montserrat_20, &lv_font_montserrat_14, bs_white, bs_dark, bs_danger);
      return;
    }
    strcpy(tj_target->name, lv_textarea_get_text(btj_modal_tjNameTextArea));
    log_d("oldname %s usedname %s targetname %s", tj_oldName, tj_used.name, tj_target->name);
    if (!changed) {
      modal_create_alert("Tidak ada perubahan dalam template jadwal!", "Peringatan!");
      return;
    }
    if (strcmp(tj_oldName, tj_target->name) != 0) { // TemplateJadwal is renamed, so rename the binary and folder for the specified TemplateJadwal
      bool updateUsedTJ = strcmp(tj_oldName, tj_used.name) == 0;
      char pathFrom[64] = { 0 };
      char pathTo[64] = { 0 };
      sprintf(pathFrom, PATH_TJ"%s.bin", tj_oldName);
      sprintf(pathTo, PATH_TJ"%s.bin", tj_target->name);
      log_d("rename file from %s to %s", pathFrom, pathTo);
      if (!ESPSYS_FS.rename(pathFrom, pathTo)) {
        modal_create_alert("Gagal menyimpan template jadwal!\nGagal rename file biner", "Gagal!", &lv_font_montserrat_20, &lv_font_montserrat_14, bs_white, bs_dark, bs_danger);
        return;
      }
      sprintf(pathFrom, PATH_TJ"%s", tj_oldName);
      sprintf(pathTo, PATH_TJ"%s", tj_target->name);
      log_d("rename folder from %s to %s", pathFrom, pathTo);
      if (!ESPSYS_FS.rename(pathFrom, pathTo)) {
        modal_create_alert("Gagal menyimpan template jadwal!\nGagal rename folder biner", "Gagal!", &lv_font_montserrat_20, &lv_font_montserrat_14, bs_white, bs_dark, bs_danger);
        return;
      }
      if (updateUsedTJ) {
        if (!templateJadwal_activeName_update(tj_target->name)) {
          modal_create_alert("Gagal menyimpan template jadwal!\nGagal update template jadwal aktif", "Gagal!", &lv_font_montserrat_20, &lv_font_montserrat_14, bs_white, bs_dark, bs_danger);
          return;
        }
        templateJadwal_activeName_load();
      }
      strcpy(tj_oldName, tj_target->name);
    }
    changed = !templateJadwal_store(tj_target);
    if (changed)
      modal_create_alert("Gagal menyimpan template jadwal!", "Gagal!", &lv_font_montserrat_20, &lv_font_montserrat_14, bs_white, bs_dark, bs_danger);
    else {
      modal_create_alert("Sukses menyimpan template jadwal!", "Sukses", &lv_font_montserrat_20, &lv_font_montserrat_14, bs_white, bs_dark, bs_success);

      char tempName[FS_MAX_NAME_LEN] = { 0 };
      strcpy(tempName, tj_target->name);
      templateJadwal_list_load(); // Reload the tj_list because one of TemplateJadwal is changed
      for (int i = 0; i < TJ_MAX_LEN;i++) { // Make sure the pointer for tj_target is still pointing to the edited TemplateJadwal
        if (strcmp(tj_lists[i].name, tempName) == 0) {
          tj_target = &tj_lists[i];
          break;
        }
      }

      tj_table_build(); // Reload the template jadwal list on tab two
      tabelJadwalHariIni(); // Update tabelJadwalHariIni on tab one just in case the tabel bel is changed
    }
    log_d("Done Saving TJ");
    }, LV_EVENT_CLICKED, NULL);

  // Textarea for TemplateJadwal name
  btj_modal_tjNameTextArea = lv_textarea_create(btj_modal);
  initTextArea(btj_modal_tjNameTextArea, 15, 70);
  lv_textarea_set_text(btj_modal_tjNameTextArea, tj_oldName);
  lv_obj_t* componentLabel = lv_label_create(btj_modal);
  initComponentLabel(componentLabel, btj_modal_tjNameTextArea, "Nama Template Jadwal :");
  lv_obj_add_event_cb(btj_modal_tjNameTextArea, [](lv_event_t* e) { // Text area focus/defocus event (show/hide keyboard)
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* ta = lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED) {
      lv_obj_set_height(btj_overlay, LV_VER_RES - lv_obj_get_height(regularKeyboard));
      lv_obj_update_layout(btj_overlay);   /*Be sure the sizes are recalculated*/
      lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
      lv_keyboard_set_textarea(regularKeyboard, ta);
      lv_obj_clear_flag(regularKeyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (code == LV_EVENT_DEFOCUSED) {
      lv_keyboard_set_textarea(regularKeyboard, NULL);
      lv_obj_set_height(btj_overlay, LV_VER_RES);
      lv_obj_update_layout(btj_overlay);   /*Be sure the sizes are recalculated*/
      lv_obj_add_flag(regularKeyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
      changed = true;
    }
    }, LV_EVENT_ALL, NULL);

  // Textarea for TemplateJadwal tipe
  btj_modal_tjTypeBtn = lv_btn_create(btj_modal);
  lv_obj_t* btnLabel = lvc_btn_init(btj_modal_tjTypeBtn, "", LV_ALIGN_TOP_LEFT, 15, 130, &lv_font_montserrat_14, bs_indigo_500, bs_white, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_SCROLL, lv_pct(100), lv_pct(50));
  lv_obj_set_style_pad_all(btj_modal_tjTypeBtn, 5, 0);
  lv_label_set_text(btnLabel, tj_target->tipeJadwal == TJ_HARIAN ? "Harian" : "Mingguan");
  componentLabel = lv_label_create(btj_modal);
  initComponentLabel(componentLabel, btj_modal_tjTypeBtn, "Tipe Jadwal : ");
  lv_obj_add_event_cb(btj_modal_tjTypeBtn, [](lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    lv_obj_t* btnLabel = (lv_obj_t*)lv_event_get_user_data(e);
    btj_wpd.issuer = btn;
    lv_obj_t* rollpick = rollpick_create(&btj_wpd, "Tipe Jadwal", tipeOptions, &lv_font_montserrat_20);
    lv_roller_set_selected(rollpick, strcmp(lv_label_get_text(btnLabel), "Mingguan") == 0, LV_ANIM_OFF);
    }, LV_EVENT_CLICKED, btnLabel);
  lv_obj_add_event_cb(btj_modal_tjTypeBtn, [](lv_event_t* e) {
    changed = true;
    lv_obj_t* btn = lv_event_get_target(e);
    lv_obj_t* btnLabel = (lv_obj_t*)lv_event_get_user_data(e);
    WidgetParameterData* wpd = (WidgetParameterData*)lv_event_get_param(e);
    tj_target->tipeJadwal = *(bool*)wpd->param;
    lv_label_set_text(btnLabel, *(bool*)wpd->param ? "Mingguan" : "Harian");
    selectedRollerIdx = *(bool*)wpd->param ? 0 : 7;
    lv_event_send(btj_modal_tjHariBtn, LV_EVENT_REFRESH, wpd);
    }, LV_EVENT_REFRESH, btnLabel);

  lv_obj_t* line = lv_line_create(btj_modal);
  static const lv_point_t line_points[] = { {.x = 10,.y = 180},{.x = 434,.y = 180} };
  lv_line_set_points(line, line_points, 2);
  lv_obj_set_style_line_width(line, 2, 0);
  lv_obj_set_style_line_color(line, bs_gray_400, 0);

  // Textarea for TemplateJadwal JadwalHarian day choice
  btj_modal_tjHariBtn = lv_btn_create(btj_modal);
  btnLabel = lvc_btn_init(btj_modal_tjHariBtn, "", LV_ALIGN_TOP_LEFT, 15, 200, &lv_font_montserrat_14, bs_indigo_500, bs_white, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_SCROLL, lv_pct(100), lv_pct(50));
  lv_obj_set_style_pad_all(btj_modal_tjHariBtn, 5, 0);
  lv_label_set_text(btnLabel, tj_target->tipeJadwal == TJ_HARIAN ? "Setiap Hari" : "Minggu");
  if (tj_target->tipeJadwal == TJ_HARIAN)
    lv_obj_add_state(btj_modal_tjHariBtn, LV_STATE_DISABLED);
  lv_obj_add_event_cb(btj_modal_tjHariBtn, [](lv_event_t* e) {
    lv_obj_t* btj_modal_tjHariBtn = lv_event_get_target(e);
    lv_obj_t* btnLabel = (lv_obj_t*)lv_event_get_user_data(e);
    if (strcmp(lv_label_get_text(btnLabel), "Setiap Hari") == 0)
      return;
    if (belChanged == false) {
      btj_wpd.issuer = btj_modal_tjHariBtn;
      lv_obj_t* rollpick = rollpick_create(&btj_wpd, "Pilih Hari", hariOptions, &lv_font_montserrat_20);
      lv_roller_set_selected(rollpick, strToDow(lv_label_get_text(btnLabel)), LV_ANIM_OFF);
      return;
    }
    else {
      btj_wpd.issuer = btnLabel;
      modal_create_confirm(&btj_wpd, "Terdapat perubahan dalam daftar bel, Apakah anda yakin untuk mengganti hari daftar bel?\nKlik \"OK\" untuk membuang perubahan.\nKlik tombol \"Simpan bel\" apabila ingin menyimpan perubahan dalam tabel bel", "Peringatan", &lv_font_montserrat_16, &lv_font_montserrat_20, bs_dark, bs_dark, bs_warning, "Ok", "Batal", lv_pct(80), lv_pct(90));
    }
    }, LV_EVENT_CLICKED, btnLabel);
  lv_obj_add_event_cb(btnLabel, [](lv_event_t* e) {
    lv_obj_t* btnLabel = (lv_obj_t*)lv_event_get_user_data(e);
    btj_wpd.issuer = btj_modal_tjHariBtn;
    lv_obj_t* rollpick = rollpick_create(&btj_wpd, "Pilih Hari", hariOptions, &lv_font_montserrat_20);
    lv_roller_set_selected(rollpick, strToDow(lv_label_get_text(btnLabel)), LV_ANIM_OFF);
    }, LV_EVENT_REFRESH, btnLabel);
  lv_obj_add_event_cb(btj_modal_tjHariBtn, [](lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    lv_obj_t* btnLabel = (lv_obj_t*)lv_event_get_user_data(e);
    WidgetParameterData* wpd = (WidgetParameterData*)lv_event_get_param(e);
    uint16_t selectedIdx = *(uint16_t*)wpd->param;
    if (selectedIdx <= 6)
      lv_obj_clear_state(btj_modal_tjHariBtn, LV_STATE_DISABLED);
    else
      lv_obj_add_state(btj_modal_tjHariBtn, LV_STATE_DISABLED);

    lv_label_set_text(btnLabel, dowToStr(selectedIdx));

    jadwalHari_load(tj_target, jw_temp, selectedIdx <= 6 ? selectedIdx : 0);

    btj_tj_build();
    }, LV_EVENT_REFRESH, btnLabel);
  componentLabel = lv_label_create(btj_modal);
  initComponentLabel(componentLabel, btj_modal_tjHariBtn, "Daftar Bel Untuk Hari :");

  btj_modal_saveBellBtn = lv_btn_create(btj_modal);
  lvc_btn_init(btj_modal_saveBellBtn, "Simpan Bel", LV_ALIGN_TOP_RIGHT, -15, 195);
  lv_obj_add_event_cb(btj_modal_saveBellBtn, [](lv_event_t* e) {
    if (!belChanged) {
      modal_create_alert("Tidak ada perubahan dalam tabel bel!", "Peringatan!");
      return;
    }
    lv_obj_t* btnLabel = (lv_obj_t*)lv_event_get_user_data(e);
    belChanged = !jadwalHari_store(tj_target, jw_temp, tj_target->tipeJadwal == TJ_HARIAN ? 0 : strToDow(lv_label_get_text(btnLabel)));
    if (belChanged)
      modal_create_alert("Gagal menyimpan tabel bel!", "Gagal!", &lv_font_montserrat_20, &lv_font_montserrat_14, bs_white, bs_dark, bs_danger);
    else
      modal_create_alert("Sukses menyimpan tabel bel!", "Sukses", &lv_font_montserrat_20, &lv_font_montserrat_14, bs_white, bs_dark, bs_success);

    tabelJadwalHariIni(); // Update tabelJadwalHariIni just in case the changed tabel bel is used for tabelJadwalHariIni
    }, LV_EVENT_CLICKED, btnLabel);

  jadwalHari_load(tj_target, jw_temp, 0);

  btj_tj_build(false);
}
void TemplateJadwalBuilder::btj_tj_build(bool refresh) {
  if (refresh) {
    lv_obj_del(btj_modal_bellList);
    lv_obj_del(btj_dummyHeight);
    lv_obj_del(btj_modal_addBellBtn);
  }

  btj_modal_bellList = lv_table_create(btj_modal); // Uses table to save memory
  lv_obj_set_size(btj_modal_bellList, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_translate_y(btj_modal_bellList, 100, 0);
  lv_obj_set_style_pad_all(btj_modal_bellList, 0, LV_PART_MAIN);
  lv_obj_add_style(btj_modal_bellList, &style_noBorder, LV_PART_MAIN);
  lv_obj_set_style_translate_y(btj_modal_bellList, 250, 0);

  lv_table_set_col_cnt(btj_modal_bellList, 5);
  lv_table_set_row_cnt(btj_modal_bellList, jw_temp->jumlahBel + 1);

  lv_obj_set_style_pad_top(btj_modal_bellList, 8, LV_PART_ITEMS);
  lv_obj_set_style_pad_bottom(btj_modal_bellList, 8, LV_PART_ITEMS);
  lv_obj_set_style_pad_right(btj_modal_bellList, 0, LV_PART_ITEMS);
  lv_obj_set_style_pad_left(btj_modal_bellList, 0, LV_PART_ITEMS);

  lv_obj_set_style_text_align(btj_modal_bellList, LV_TEXT_ALIGN_CENTER, LV_PART_ITEMS | LV_STATE_DEFAULT);

  btj_modal_addBellBtn = lv_btn_create(btj_modal);
  lv_obj_t* label = lvc_btn_init(btj_modal_addBellBtn, LV_SYMBOL_PLUS);
  lv_obj_set_size(btj_modal_addBellBtn, 50, 50);
  lv_obj_set_style_radius(btj_modal_addBellBtn, lv_pct(100), 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  lv_obj_add_event_cb(btj_modal_addBellBtn, [](lv_event_t* e) {
    if (jw_temp->jumlahBel == 30) {
      modal_create_alert("Tidak dapat menambah bel lagi!", "Peringatan!", &lv_font_montserrat_20, &lv_font_montserrat_14, bs_white, bs_dark, bs_danger);
      return;
    }
    belChanged = true;
    strcpy(jw_temp->namaBel[jw_temp->jumlahBel], "");
    strcpy(jw_temp->belAudioFile[jw_temp->jumlahBel], "");
    jw_temp->jadwalBel[jw_temp->jumlahBel] = 0;
    jw_temp->jumlahBel++;
    lv_table_set_row_cnt(btj_modal_bellList, jw_temp->jumlahBel + 1);
    lv_table_set_cell_value_fmt(btj_modal_bellList, jw_temp->jumlahBel, 0, "%d", jw_temp->jumlahBel);
    lv_table_set_cell_value_fmt(btj_modal_bellList, jw_temp->jumlahBel, 1, jw_temp->namaBel[jw_temp->jumlahBel - 1]);
    lv_table_set_cell_value_fmt(btj_modal_bellList, jw_temp->jumlahBel, 2, "%02d:%02d", jw_temp->jadwalBel[jw_temp->jumlahBel - 1] / 100, jw_temp->jadwalBel[jw_temp->jumlahBel - 1] % 100);
    lv_table_set_cell_value_fmt(btj_modal_bellList, jw_temp->jumlahBel, 3, jw_temp->belAudioFile[jw_temp->jumlahBel - 1]);
    lv_obj_set_size(btj_modal_bellList, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align_to(btj_modal_addBellBtn, btj_modal_bellList, LV_ALIGN_OUT_BOTTOM_MID, 0, 30);
    lv_obj_align_to(btj_dummyHeight, btj_modal_addBellBtn, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_update_layout(btj_modal_bellList);
    lv_obj_update_layout(btj_modal_addBellBtn);
    lv_obj_update_layout(btj_dummyHeight);
    // lv_coord_t tempScroll = lv_obj_get_scroll_y(btj_modal); // Save the scroll
    // btj_tj_build();
    // lv_obj_scroll_to_y(btj_modal, tempScroll, LV_ANIM_OFF); // Scroll to the saved position immediately
    }, LV_EVENT_CLICKED, NULL);

  btj_dummyHeight = lv_obj_create(btj_modal);
  lv_obj_set_size(btj_dummyHeight, 1, 30);
  lv_obj_set_style_opa(btj_dummyHeight, 0, 0);

  for (int i = 0;i < 5;i++) {
    lv_table_set_cell_value(btj_modal_bellList, 0, i, btjListHeader[i]);
    lv_table_set_col_width(btj_modal_bellList, i, btjListWidthDescriptor[i]);
  }

  for (int i = 0; i < jw_temp->jumlahBel;i++) {
    lv_table_set_cell_value_fmt(btj_modal_bellList, i + 1, 0, "%d", i + 1);
    lv_table_set_cell_value_fmt(btj_modal_bellList, i + 1, 1, jw_temp->namaBel[i]);
    lv_table_set_cell_value_fmt(btj_modal_bellList, i + 1, 2, "%02d:%02d", jw_temp->jadwalBel[i] / 100, jw_temp->jadwalBel[i] % 100);
    lv_table_set_cell_value_fmt(btj_modal_bellList, i + 1, 3, jw_temp->belAudioFile[i]);
  }
  lv_obj_align_to(btj_modal_addBellBtn, btj_modal_bellList, LV_ALIGN_OUT_BOTTOM_MID, 0, 30);
  lv_obj_align_to(btj_dummyHeight, btj_modal_addBellBtn, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
  lv_obj_update_layout(btj_modal_addBellBtn);
  lv_obj_update_layout(btj_dummyHeight);

  lv_obj_add_event_cb(btj_modal_bellList, btj_table_draw_cb, LV_EVENT_DRAW_PART_END, NULL); // Callback used to draw the pseudo button on the table (table can't draw object)
  lv_obj_add_event_cb(btj_modal_bellList, btj_table_actionBtn_cb, LV_EVENT_VALUE_CHANGED, NULL); // Callback for action button click
  lv_obj_add_event_cb(btj_modal_bellList, [](lv_event_t* e) {
    const char* traverserParam = (const char*)lv_event_get_param(e);
    lv_table_set_cell_value_fmt(btj_modal_bellList, ta_row, ta_col, traverserParam);
    strcpy(jw_temp->belAudioFile[ta_row - 1], traverserParam);
    belChanged = true;
    lv_obj_set_size(btj_modal_bellList, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align_to(btj_modal_addBellBtn, btj_modal_bellList, LV_ALIGN_OUT_BOTTOM_MID, 0, 30);
    lv_obj_align_to(btj_dummyHeight, btj_modal_addBellBtn, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_update_layout(btj_modal_bellList);
    lv_obj_update_layout(btj_modal_addBellBtn);
    lv_obj_update_layout(btj_dummyHeight);
    }, LV_EVENT_REFRESH, NULL); // Callback for action button click
}
void TemplateJadwalBuilder::create_textarea_prompt(const char* placeholder) {
  lv_obj_t* overlay = lvc_create_overlay();

  lv_obj_t* modal = lv_obj_create(overlay);
  lv_obj_set_size(modal, 240, 160);
  lv_obj_set_style_pad_all(modal, 0, 0);
  lv_obj_center(modal);

  lv_obj_t* header = lv_label_create(modal);
  lvc_label_init(header, &lv_font_montserrat_16, LV_ALIGN_TOP_LEFT, 13, 15);
  lv_label_set_text_static(header, "Ubah Nilai");

  lv_obj_t* ta = lv_textarea_create(modal);
  initTextArea(ta, 0, 0);
  lv_obj_align(ta, LV_ALIGN_CENTER, 0, -10);
  lv_obj_set_width(ta, lv_pct(80));
  lv_textarea_set_text(ta, ta_col == 1 ? placeholder : "");
  if (ta_col == 2) {
    lv_textarea_set_max_length(ta, 4);
    lv_textarea_set_accepted_chars(ta, "0123456789");
  }

  lv_obj_remove_event_cb(ta_col == 2 ? numericKeyboard : regularKeyboard, kb_event_cb);
  lv_obj_add_event_cb(ta_col == 2 ? numericKeyboard : regularKeyboard, kb_event_cb, LV_EVENT_ALL, overlay);
  lv_obj_add_event_cb(ta, [](lv_event_t* e) {
    lv_obj_t* ta = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* overlay = lv_obj_get_parent(lv_obj_get_parent(ta));
    lv_obj_t* kb = (lv_obj_t*)lv_event_get_user_data(e);
    if (code == LV_EVENT_FOCUSED) {
      lv_obj_set_height(overlay, LV_VER_RES - lv_obj_get_height(kb));
      lv_obj_update_layout(overlay);   /*Be sure the sizes are recalculated*/
      lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
      lv_keyboard_set_textarea(kb, ta);
      lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(kb);
      lv_obj_update_layout(kb);
    }
    else if (code == LV_EVENT_DEFOCUSED) {
      lv_obj_set_height(overlay, LV_VER_RES);
      lv_obj_update_layout(overlay);   /*Be sure the sizes are recalculated*/
      lv_keyboard_set_textarea(kb, NULL);
      lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
      lv_obj_update_layout(kb);
    }
    }, LV_EVENT_ALL, ta_col == 2 ? numericKeyboard : regularKeyboard);
  lv_event_send(ta, LV_EVENT_FOCUSED, ta_col == 2 ? numericKeyboard : regularKeyboard);

  lv_obj_t* cancelButton = lv_btn_create(modal);
  lvc_btn_init(cancelButton, "Batal", LV_ALIGN_BOTTOM_RIGHT, -15, -15);
  lv_obj_add_event_cb(cancelButton, [](lv_event_t* e) {
    lv_obj_t* ta = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_t* overlay = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
    lv_event_send(ta, LV_EVENT_DEFOCUSED, ta_col == 2 ? numericKeyboard : regularKeyboard);
    lv_obj_del(overlay);
    }, LV_EVENT_CLICKED, ta);

  lv_obj_t* okButton = lv_btn_create(modal);
  lvc_btn_init(okButton, "Ok", LV_ALIGN_BOTTOM_RIGHT, -100, -15);
  lv_obj_add_event_cb(okButton, [](lv_event_t* e) {
    lv_obj_t* overlay = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
    lv_obj_t* ta = (lv_obj_t*)lv_event_get_user_data(e);
    if (ta_col == 1) {
      lv_table_set_cell_value(btj_modal_bellList, ta_row, ta_col, lv_textarea_get_text(ta));
      strcpy(jw_temp->namaBel[ta_row - 1], lv_textarea_get_text(ta));
    }
    else {
      int numval = atoi(lv_textarea_get_text(ta));
      lv_table_set_cell_value_fmt(btj_modal_bellList, ta_row, ta_col, "%02d:%02d", numval / 100, numval % 100);
      jw_temp->jadwalBel[ta_row - 1] = numval;
    }
    lv_event_send(ta, LV_EVENT_DEFOCUSED, ta_col == 2 ? numericKeyboard : regularKeyboard);
    belChanged = true;
    lv_obj_del(overlay);
    lv_obj_update_layout(btj_modal_bellList);
    }, LV_EVENT_CLICKED, ta);
}
void TemplateJadwalBuilder::btj_table_actionBtn_cb(lv_event_t* e) {
  lv_obj_t* obj = lv_event_get_target(e);
  uint16_t col;
  uint16_t row;
  lv_table_get_selected_cell(obj, &row, &col);
  if (row == 0 || col == 0)
    return;
  ta_row = row;
  ta_col = col;
  if (col == 4) { // Delete button
    row--;
    memmove(jw_temp->belAudioFile + row, jw_temp->belAudioFile + row + 1, (jw_temp->jumlahBel - row - 1) * sizeof(*jw_temp->belAudioFile));
    memmove(jw_temp->namaBel + row, jw_temp->namaBel + row + 1, (jw_temp->jumlahBel - row - 1) * sizeof(*jw_temp->namaBel));
    memmove(jw_temp->jadwalBel + row, jw_temp->jadwalBel + row + 1, (jw_temp->jumlahBel - row - 1) * sizeof(*jw_temp->jadwalBel));
    jw_temp->jumlahBel--;
    belChanged = true;
    lv_obj_update_layout(btj_modal_bellList);
    lv_coord_t tempScroll = lv_obj_get_scroll_y(btj_modal); // Save the scroll
    btj_tj_build();
    lv_obj_scroll_to_y(btj_modal, tempScroll, LV_ANIM_OFF); // Scroll to the saved position immediately
  }
  else if (col == 1 || col == 2) {
    create_textarea_prompt(lv_table_get_cell_value(obj, row, col));
  }
  else
    Traverser::createTraverser(btj_modal_bellList, "/");
}
void TemplateJadwalBuilder::btj_table_draw_cb(lv_event_t* e) {
  lv_obj_t* obj = lv_event_get_target(e);
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(e);
  // id = current row × col count + current column
  uint32_t row = dsc->id / lv_table_get_col_cnt(obj);
  uint32_t col = dsc->id - row * lv_table_get_col_cnt(obj);
  if (dsc->part == LV_PART_ITEMS && col == 4 && row > 0) { // Draw the button only on column 5 of table and row > 0
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    lv_draw_rect_dsc_init(&rect_dsc);
    label_dsc.font = &lv_font_montserrat_12;
    label_dsc.color = bs_white;
    label_dsc.align = LV_TEXT_ALIGN_CENTER; // Used for centering text horizontally

    rect_dsc.bg_color = bs_danger;

    rect_dsc.radius = 6; // Round the rectangular a little bit

    // Draw the rectangular area for button
    lv_area_t sw_area;
    // button length is 100% - 15px padding on left/right side
    sw_area.x1 = dsc->draw_area->x1 + 15;
    sw_area.x2 = dsc->draw_area->x2 - 15;
    // button height is 20px centered from cell height with 10px padding on top side
    int drawHeight = (dsc->draw_area->y2 - dsc->draw_area->y1);
    sw_area.y1 = dsc->draw_area->y1 + (drawHeight / 2) - 10;
    sw_area.y2 = sw_area.y1 + 20;
    label_dsc.ofs_y = ((sw_area.y2 - sw_area.y1) / 2) - 6; // Center text vertically
    lv_draw_rect(dsc->draw_ctx, &rect_dsc, &sw_area); // Draw rectangular for button

    lv_draw_label(dsc->draw_ctx, &label_dsc, &sw_area, "Hapus", NULL);
  }
}
void TemplateJadwalBuilder::initTextArea(lv_obj_t* ta, lv_coord_t offsetx, lv_coord_t offsety) {
  lv_textarea_set_max_length(ta, FS_MAX_NAME_LEN - 1); // Maximum input text length of the TemplateJadwal name is 31 character
  lv_textarea_set_one_line(ta, true);
  lv_obj_align(ta, LV_ALIGN_TOP_LEFT, offsetx, offsety);
  lv_obj_set_width(ta, lv_pct(50));
  lv_obj_set_style_pad_all(ta, 5, LV_PART_MAIN);
}
void TemplateJadwalBuilder::initComponentLabel(lv_obj_t* label, lv_obj_t* alignTo, const char* labelMessage) {
  lv_obj_align_to(label, alignTo, LV_ALIGN_OUT_TOP_LEFT, 0, 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
  lv_label_set_text_static(label, labelMessage);
}


lv_obj_t* lvc_create_overlay() {
  lv_obj_t* overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(overlay, 480, 320);
  lv_obj_add_style(overlay, &style_noBorder, 0);
  lv_obj_add_style(overlay, &style_zeroRadius, 0);
  lv_obj_set_style_bg_color(overlay, bs_dark, 0);
  lv_obj_set_style_bg_opa(overlay, 76, 0); // 30% opacity
  return overlay;
}

lv_obj_t* lvc_btn_init(lv_obj_t* btn, const char* labelText,
  lv_align_t align, lv_coord_t offsetX, lv_coord_t offsetY,
  const lv_font_t* font, lv_color_t bgColor, lv_color_t textColor,
  lv_text_align_t alignText, lv_label_long_mode_t longMode, lv_coord_t labelWidth,
  lv_coord_t btnSizeX, lv_coord_t btnSizeY) {
  lv_obj_t* label = lv_label_create(btn);
  lv_label_set_text_static(label, labelText);
  lv_obj_align(btn, align, offsetX, offsetY);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_bg_color(btn, bgColor, 0);
  lv_obj_set_style_text_color(label, textColor, 0);
  lv_obj_set_style_text_align(label, alignText, 0);
  lv_label_set_long_mode(label, longMode);
  if (labelWidth != 0) lv_obj_set_width(label, labelWidth);
  lv_obj_center(label); // Center the label
  if (labelWidth != 0)
    lv_obj_set_width(label, labelWidth);
  if (btnSizeX != 0)
    lv_obj_set_width(btn, btnSizeX);
  if (btnSizeY != 0)
    lv_obj_set_height(btn, btnSizeY);
  return label;
}

void lvc_label_init(lv_obj_t* label, const lv_font_t* font,
  lv_align_t align, lv_coord_t offsetX, lv_coord_t offsetY,
  lv_color_t textColor, lv_text_align_t alignText, lv_label_long_mode_t longMode, lv_coord_t textWidth) {
  lv_obj_set_style_text_color(label, textColor, 0);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_align(label, alignText, 0);
  lv_obj_align(label, align, offsetX, offsetY);
  if (longMode != LV_LABEL_LONG_WRAP) // Set long mode if set value is not defaulted
    lv_label_set_long_mode(label, longMode);
  if (textWidth != 0) // Only set label width if passed textWidth value is not 0
    lv_obj_set_width(label, textWidth);
}

void lvc_obj_set_pad_wrapper(lv_obj_t* obj, lv_coord_t padTop, lv_coord_t padBottom, lv_coord_t padLeft, lv_coord_t padRight, lv_style_selector_t selector) {
  if (padTop != 0xFF) lv_obj_set_style_pad_top(obj, padTop, 0);
  if (padBottom != 0xFF) lv_obj_set_style_pad_bottom(obj, padBottom, 0);
  if (padRight != 0xFF) lv_obj_set_style_pad_right(obj, padRight, 0);
  if (padLeft != 0xFF) lv_obj_set_style_pad_left(obj, padLeft, 0);
}

void initStyles() {
  lv_style_init(&style_zeroRadius);
  lv_style_set_radius(&style_zeroRadius, 0);

  lv_style_init(&style_noBorder);
  lv_style_set_border_width(&style_noBorder, 0);

  lv_style_init(&style_flexRow);
  lv_style_set_flex_flow(&style_flexRow, LV_FLEX_FLOW_ROW_WRAP);
  lv_style_set_layout(&style_flexRow, LV_LAYOUT_FLEX);

  lv_style_init(&style_thinBottomBorder);
  lv_style_set_border_color(&style_thinBottomBorder, bs_gray_400);
  lv_style_set_border_width(&style_thinBottomBorder, 1);
  lv_style_set_border_side(&style_thinBottomBorder, LV_BORDER_SIDE_BOTTOM);

  lv_style_init(&scr1Bg);
  lv_style_set_bg_color(&scr1Bg, bs_indigo_500);
  lv_style_set_bg_grad_color(&scr1Bg, bs_indigo_700);
  lv_style_set_bg_grad_dir(&scr1Bg, LV_GRAD_DIR_VER);
  lv_style_set_bg_dither_mode(&scr1Bg, LV_DITHER_ERR_DIFF);
}

float volumeToGain(uint8_t volume)
{ // Range is 0 to I2S_VOLUME_STEP
  if (volume == 0)
    return 0.;
  else if (volume > uint8_t(I2S_VOLUME_STEP))
    volume = uint8_t(I2S_VOLUME_STEP);
  return (float(volume - 1) * gainPerVolumeStep) + I2S_MIN_GAIN;
}

const char* dowToStr(int dow) {
  return (dow == 0) ? "Minggu" :
    (dow == 1) ? "Senin" :
    (dow == 2) ? "Selasa" :
    (dow == 3) ? "Rabu" :
    (dow == 4) ? "Kamis" :
    (dow == 5) ? "Jumat" :
    (dow == 6) ? "Sabtu" : "Setiap Hari";
}

const char* monthToStr(int month) {
  return (month == 1) ? "Januari" :
    (month == 2) ? "Februari" :
    (month == 3) ? "Maret" :
    (month == 4) ? "April" :
    (month == 5) ? "Mei" :
    (month == 6) ? "Juni" :
    (month == 7) ? "Juli" :
    (month == 8) ? "Agustus" :
    (month == 9) ? "September" :
    (month == 10) ? "Oktober" :
    (month == 11) ? "November" : "Desember";
}

uint8_t strToDow(const char* str) {
  return (strcmp(str, "Minggu") == 0) ? 0 :
    (strcmp(str, "Senin") == 0) ? 1 :
    (strcmp(str, "Selasa") == 0) ? 2 :
    (strcmp(str, "Rabu") == 0) ? 3 :
    (strcmp(str, "Kamis") == 0) ? 4 :
    (strcmp(str, "Jumat") == 0) ? 5 : 6;
}
