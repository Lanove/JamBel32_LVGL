#include "lvgl.h"
#include "colors.h"
#include "keyboard_maps.h"
#include "lgfx_config.h"
#include "lvgl_core.h"
#include <SPIFFS.h>
#include <SD.h>
#include <Wire.h>
#include <RTClib.h>

#define VERBOSE_FLASH_MODE(MODE) MODE == FM_QIO ? "FM_QIO" : MODE == FM_QOUT    ? "FM_QOUT"      \
                                                         : MODE == FM_DIO       ? "FM_DIO"       \
                                                         : MODE == FM_DOUT      ? "FM_DOUT"      \
                                                         : MODE == FM_FAST_READ ? "FM_FAST_READ" \
                                                         : MODE == FM_SLOW_READ ? "FM_SLOW_READ" \
                                                         : MODE == FM_UNKNOWN   ? "FM_UNKNOWN"   \
                                                                                : ""
#define VERBOSE_SD_TYPE(TYPE) TYPE == CARD_NONE ? "NONE" : TYPE == CARD_MMC   ? "MMC"     \
                                                       : TYPE == CARD_SD      ? "SD"      \
                                                       : TYPE == CARD_SDHC    ? "SDHC"    \
                                                       : TYPE == CARD_UNKNOWN ? "UNKNOWN" \
                                                                              : ""

#define PATH_ESPSYS "/"
#define PATH_TJ "espsys/tj/"

#define MAX_BELL 30
#define MAX_TEMPLATE_JADWAL 10
#define TJ_HARIAN 0
#define TJ_MINGGUAN 1
#define FS_MAX_NAME_LEN 32
#define TJ_MAX_LEN 10
#define I2C_SDA GPIO_NUM_26
#define I2C_SCL GPIO_NUM_25
#define I2C_FREQ 100000U
#define ESPSYS_FS SPIFFS

#define SD_DI GPIO_NUM_33
#define SD_DO GPIO_NUM_35
#define SD_CS GPIO_NUM_14
#define SD_CLK GPIO_NUM_32
#define SDSPI_FREQUENCY 16000000U

LV_FONT_DECLARE(Montserrat_SemiBold91)
LV_FONT_DECLARE(Montserrat_SemiBold48)

static lv_style_t style_zeroRadius;
static lv_style_t style_noBorder;
static lv_style_t style_flexRow;
static lv_style_t style_thinBottomBorder;

static lv_obj_t* mainScreen;
static lv_obj_t* mainScreen_clock;
static lv_obj_t* mainScreen_date;
static lv_obj_t* mainScreen_tjName;
static lv_obj_t* mainScreen_nextBellClock;
static lv_obj_t* mainScreen_nextBellName;
static lv_obj_t* mainScreen_nextBellAudioFile;

static lv_obj_t* jw_lv_list_table;
static lv_obj_t* tj_lv_list_table;

static lv_obj_t* mainMenu;
static lv_obj_t* tab1;
static lv_obj_t* tab2;
static lv_obj_t* tab3;
static lv_obj_t* tabv;

static lv_obj_t* tab1_namaHari;
static lv_obj_t* tab1_namaTj;

static lv_obj_t* numericKeyboard;
static lv_obj_t* regularKeyboard;

static lv_style_t scr1Bg;

struct WidgetParameterData {
    lv_obj_t* issuer;
    void* param;
};

struct BelManual {
    bool enabled = false;
    char name[32] = "Kosong";
    char audioFile[128] = "Pilih File Audio";
    lv_obj_t* buttonPointer;
};

struct JadwalHari {
    char namaBel[MAX_BELL][FS_MAX_NAME_LEN];
    uint32_t jadwalBel[MAX_BELL];
    char belAudioFile[MAX_BELL][128];
    uint8_t jumlahBel;
};

struct TemplateJadwal {
    char name[FS_MAX_NAME_LEN];
    bool tipeJadwal;
    TemplateJadwal(const char* _name = "", bool _tipeJadwal = 0) {
        strcpy(name,_name);
        tipeJadwal = _tipeJadwal;
    }
};

const JadwalHari jw_empty({});
const TemplateJadwal tj_empty("new", 0);
JadwalHari jw_used; // Current used jadwal harian
JadwalHari jw_temp; // Used for storing temporary data while editing jadwal harian at menu
TemplateJadwal tj_lists[TJ_MAX_LEN];
TemplateJadwal tj_used; // Currently used template jadwal
TemplateJadwal tj_temp;
static constexpr size_t belManual_len = 4;
BelManual belManual[belManual_len];
uint8_t tj_total_active;
char tj_active_name[FS_MAX_NAME_LEN];
WidgetParameterData tj_modalConfirmData;
int tj_issue_row;
char tj_delete_confirm_message[128];

static uint8_t nextBelIndex = 0;
static uint8_t lastNextBelIndex = 255;

void lvc_label_init(lv_obj_t* label, const lv_font_t* font = &lv_font_montserrat_14, lv_align_t align = LV_ALIGN_DEFAULT, lv_coord_t offsetX = 0, lv_coord_t offsetY = 0, lv_color_t textColor = bs_dark, lv_text_align_t alignText = LV_TEXT_ALIGN_CENTER, lv_label_long_mode_t longMode = LV_LABEL_LONG_WRAP, lv_coord_t textWidth = 0);
lv_obj_t* lvc_btn_init(lv_obj_t* btn, const char* labelText, lv_align_t align = LV_ALIGN_DEFAULT, lv_coord_t offsetX = 0,
    lv_coord_t offsetY = 0, const lv_font_t* font = &lv_font_montserrat_14,
    lv_color_t bgColor = bs_indigo_500, lv_color_t textColor = bs_white,
    lv_text_align_t alignText = LV_TEXT_ALIGN_CENTER, lv_label_long_mode_t longMode = LV_LABEL_LONG_WRAP, lv_coord_t labelWidth = 0,
    lv_coord_t btnSizeX = 0, lv_coord_t btnSizeY = 0);
lv_obj_t* lvc_create_overlay();
void lvc_obj_set_pad_wrapper(lv_obj_t* obj, lv_coord_t padTop = 0xFF, lv_coord_t padBottom = 0xFF, lv_coord_t padLeft = 0xFF, lv_coord_t padRight = 0xFF, lv_style_selector_t selector = 0);
void swipe_event_cb(lv_event_t* e);
void belManual_btn_cb(lv_event_t* e);
const char* monthToStr(int month);
const char* dowToStr(int dow);
uint8_t strToDow(const char* str);
char kb_copy_buffer[128];
void kb_custom_event_cb(lv_event_t* e);
void kb_event_cb(lv_event_t* e);
void tabelJadwalHariIni();
void tj_ganti_template_btn_cb(lv_event_t* e);
void tj_table_build();
void tj_table_actionBtn_cb(lv_event_t* e);
void tj_table_draw_cb(lv_event_t* e);
void tj_table_refresh_cb(lv_event_t* e);
void btj_build(int row = 0);
lv_obj_t* rollpick_create(WidgetParameterData* wpd, const char* headerTitle, const char* options, const lv_font_t* headerFont = &lv_font_montserrat_20, lv_coord_t width = lv_pct(70), lv_coord_t height = lv_pct(70));
bool belManual_load(BelManual *bm_target, size_t len);
bool belManual_store(BelManual *bm_target, size_t len);
bool jadwalHari_load(TemplateJadwal* tj_target, JadwalHari* jwh_target, int num);
bool jadwalHari_store(TemplateJadwal* tj_target, JadwalHari* jwh_target, int num);
bool templateJadwal_load(TemplateJadwal* tj_target, const char* path);
bool templateJadwal_store(TemplateJadwal* tj_target);
bool templateJadwal_activeName_update(const char* activeName);
bool templateJadwal_activeName_load();
bool templateJadwal_list_load();
bool templateJadwal_create(TemplateJadwal* tj_target);
bool templateJadwal_delete(TemplateJadwal* tj_target);
bool templateJadwal_activeCount_load();
bool templateJadwal_activeCount_store(int num);
bool templateJadwal_changeUsedTJ(TemplateJadwal to, bool refreshElements, bool updateBinary);

lv_obj_t* modal_create_alert(const char* message, const char* headerText = "Warning!",
    const lv_font_t* headerFont = &lv_font_montserrat_20, const lv_font_t* messageFont = &lv_font_montserrat_14,
    lv_color_t headerTextColor = bs_dark, lv_color_t textColor = bs_dark,
    lv_color_t headerColor = bs_warning,
    const char* buttonText = "Ok",
    lv_coord_t xSize = lv_pct(70), lv_coord_t ySize = lv_pct(70));
lv_obj_t* modal_create_confirm(WidgetParameterData* modalConfirmData,
    const char* message, const char* headerText = "Warning!",
    const lv_font_t* headerFont = &lv_font_montserrat_20, const lv_font_t* messageFont = &lv_font_montserrat_14,
    lv_color_t headerTextColor = bs_white, lv_color_t textColor = bs_dark,
    lv_color_t headerColor = bs_indigo_700,
    const char* confirmButtonText = "Ok", const char* cancelButtonText = "Batal",
    lv_coord_t xSize = lv_pct(70), lv_coord_t ySize = lv_pct(70));

void initStyles();
void loadMainScreen();
void loadMainMenu();
void tabOne();
void tabTwo();
void tabThree();

namespace Traverser {
#define TRAVERSER_MAX_ROW 50
#define TRAVERSER_MAX_TRAVERSING_LEN 128
    char traverseDirBuffer[TRAVERSER_MAX_TRAVERSING_LEN];
    bool exist; // Variable to store if traverser is exist

    // Traverser objects
    lv_obj_t* overlay;
    lv_obj_t* modal;
    lv_obj_t* modalTitle;
    lv_obj_t* traverseBackButton;
    lv_obj_t* traversePathLabel;
    lv_obj_t* traverseCancelButton;
    lv_obj_t* traverseBox;

    // Traverser issuer object and it's passed value
    lv_obj_t* traverserIssuer;
    char traverserReturnParam[TRAVERSER_MAX_TRAVERSING_LEN];

    lv_coord_t traverserCol_widthDescriptor[] = { 46, 218, 110, 70, LV_GRID_TEMPLATE_LAST };
    const char* traverserTableHeader[4] = { "#","Nama File", "Ukuran", "Aksi" };
    const char* traverserActionButtonLabel[2] = { "Pilih","Buka" };

    // Create traverser, build MUST be true when called first from issuer
    void createTraverser(lv_obj_t* issuer, const char* dir, bool build = true);
    // Traverse back directory, only enabled when directory is not root "/"
    void traverseBack(lv_event_t* event);
    // Function used to draw the pseudo-button on the table
    void traverserTableDrawEventCallback(lv_event_t* e);
    // Action button callback
    void traverserActionButtonClicked(lv_event_t* e);
}

namespace TemplateJadwalBuilder {
    uint16_t selectedRollerIdx = 0;
    WidgetParameterData btj_wpd({ .issuer = NULL,.param = &selectedRollerIdx });
    const char tipeOptions[] = "Harian\nMingguan";
    const char hariOptions[] = "Minggu\nSenin\nSelasa\nRabu\nKamis\nJumat\nSabtu";
    // Total pixel length must be 444px
    static const lv_coord_t btjListWidthDescriptor[] = { 44, 150, 75, 100, 75 };
    static const char* btjListHeader[] = { "#","Nama Bel", "Jadwal\nBel", "Audio Bel", "Aksi" };
    bool createNew;
    bool changed;
    bool belChanged;
    int ta_row, ta_col;

    char tj_oldName[FS_MAX_NAME_LEN];
    int tjRow;

    lv_obj_t* btj_overlay;
    lv_obj_t* btj_kb;
    lv_obj_t* btj_modal;
    lv_obj_t* btj_modal_header;
    lv_obj_t* btj_modal_saveBtn;
    lv_obj_t* btj_modal_cancelBtn;
    lv_obj_t* btj_modal_tjNameTextArea;
    lv_obj_t* btj_modal_tjTypeBtn;
    lv_obj_t* btj_modal_tjHariBtn;
    lv_obj_t* btj_modal_saveBellBtn;
    lv_obj_t* btj_modal_bellList;
    lv_obj_t* btj_modal_addBellBtn;

    lv_obj_t* btj_dummyHeight;

    TemplateJadwal* tj_target;

    void create(int row);
    void create_textarea_prompt(const char* placeholder);
    void btj_tj_build(bool refresh = true);
    void btj_table_actionBtn_cb(lv_event_t* e);
    void btj_table_draw_cb(lv_event_t* e);
    void initComponentLabel(lv_obj_t* label, lv_obj_t* alignTo, const char* labelMessage);
    void initTextArea(lv_obj_t* ta, lv_coord_t offsetx, lv_coord_t offsety);
}
