#include <Arduino.h>
#include <M5Cardputer.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <BLERemoteCharacteristic.h>
#include <FastLED.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>

//--------------------------------------------------
// BLE設定
//--------------------------------------------------
#define SERVICE_UUID "1010"
#define CHARACTERISTIC_UUID "1012"

// 接続先 M5AtomS3R のBLEアドレス
static BLEAddress targetAddress("B4:3A:45:bc:9f:7d");

BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;

bool connected = false;

//--------------------------------------------------
// 本体LED設定
//--------------------------------------------------
#define LED_PIN 21
#define NUM_LEDS 1

CRGB leds[NUM_LEDS];

//--------------------------------------------------
// SDカード設定
//--------------------------------------------------
#define SD_SPI_SCK_PIN 40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_SPI_CS_PIN 12

#define MOTION_FILE "/motion.csv"
#define TRIM_FILE   "/trim.csv"
#define CONFIG_FILE "/config.csv"

bool sdReady = false;

//--------------------------------------------------
// I2Cジョイスティック設定
//--------------------------------------------------
#define JOY_ADDR 0x63
#define JOY_SDA_PIN 2
#define JOY_SCL_PIN 1

int32_t joyX = 0;
int32_t joyY = 0;
int joyCenterX = 32768;
int joyCenterY = 32768;
bool joyReady = false;

bool joyButton = false;
bool lastJoyButton = false;

const int JOY_CURSOR_DEAD = 20000;
const int JOY_EDIT_DEAD   = 12000;
const int JOY_DIRECT_DEAD = 4000;
const int JOY_FAST_THRESHOLD = 30000;

const unsigned long JOY_REPEAT_TIME = 450;

// DIRECTモード時のサーボ可動範囲
#define JOY_SERVO_RANGE 30

//--------------------------------------------------
// モーション基本設定
//--------------------------------------------------
const int NUM_PATTERNS = 7;
const int HOME_PATTERN = 6;
const int NUM_STEPS = 50;
const int NUM_SERVOS = 6;

// パターン名
const char* patternNames[NUM_PATTERNS] = {
    "FWD", "BACK", "RIGHT", "LEFT", "RTN", "LTN", "HOME"
};

// 現在のパターン・ステップ・選択サーボ
int currentPattern = 0;
int currentStep = 0;
int selectedServo = 0;

// モーションデータ
// motion[パターン][ステップ][サーボ]
int motion[NUM_PATTERNS][NUM_STEPS][NUM_SERVOS];

// 各パターンの再生ステップ数
// HOMEは1ステップ固定
int playSteps[NUM_PATTERNS] = {
    50, 50, 50, 50, 50, 50, 1
};

//--------------------------------------------------
// トリム・オフセット
//--------------------------------------------------

// トリム角度
// 実際のサーボ中心位置調整用
int trimAngles[NUM_SERVOS] = {
    90, 90, 90, 90, 90, 90
};

// モーション側の角度オフセット
// -90〜+90
int controlOffsets[NUM_SERVOS] = {
    0, 0, 0, 0, 0, 0
};

//--------------------------------------------------
// 再生状態
//--------------------------------------------------
bool playing = false;
bool trimMode = false;

unsigned long lastPlayTime = 0;

//--------------------------------------------------
// キーボード長押し・リピート設定
//--------------------------------------------------
const unsigned long LONG_PRESS_TIME   = 400;
const unsigned long SHORT_REPEAT_TIME = 180;
const unsigned long LONG_REPEAT_TIME  = 120;

bool keyActive[256] = {false};
unsigned long keyPressStart[256] = {0};
unsigned long keyLastAction[256] = {0};

//--------------------------------------------------
// ジョイスティック操作モード
//--------------------------------------------------
enum JoyMode {
    JOY_CONTROL_MODE1,
    JOY_CONTROL_MODE2,
    JOY_DIRECT_MODE
};

JoyMode joyMode = JOY_CONTROL_MODE1;

unsigned long lastJoyAction = 0;

//--------------------------------------------------
// 顔ランダム制御
//--------------------------------------------------

// Control Mode 1 / 2 のとき、サーボ1・2をランダムに動かす
unsigned long lastFaceRandomTime = 0;
unsigned long lastFaceSmoothTime = 0;

int randomFaceTarget[2] = {0, 0};
int randomFaceCurrent[2] = {0, 0};

unsigned long nextFaceRandomInterval = 2000;

const unsigned long FACE_SMOOTH_INTERVAL = 50;
const int FACE_RANDOM_RANGE = 20;
const int FACE_SMOOTH_STEP = 2;

//--------------------------------------------------
// 関数プロトタイプ
//--------------------------------------------------
void drawDisplay();
void showSaveMessage();

void sendAngles();
void saveStep();
void loadCurrentStep();

void nextStep();
void prevStep();

void updateLED();

bool connectToServer();
void checkConnection();

void selectPattern(int index);

void selectPrevServo();
void selectNextServo();

void changeTrim(int index, int delta);
void changeControl(int index, int delta);
void changePlaySteps(int delta);

bool initSD();
bool saveDataToSD();
bool loadDataFromSD();

void handleKey(char c, bool longPress);
void handleKeyboard();

bool readJoystick();
void calibrateJoystick();
void handleJoystick();

void updateRandomFace();

//--------------------------------------------------
// SDカード初期化
//--------------------------------------------------
bool initSD() {
    SPI.begin(
        SD_SPI_SCK_PIN,
        SD_SPI_MISO_PIN,
        SD_SPI_MOSI_PIN,
        SD_SPI_CS_PIN
    );

    if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
        Serial.println("SD init failed");
        return false;
    }

    if (SD.cardType() == CARD_NONE) {
        Serial.println("No SD card");
        return false;
    }

    Serial.println("SD init OK");
    return true;
}

//--------------------------------------------------
// SDカードへ保存
//
// 保存するファイル
// motion.csv : モーションデータ
// trim.csv   : トリム値
// config.csv : 各パターンの再生ステップ数
//--------------------------------------------------
bool saveDataToSD() {
    if (!sdReady) return false;

    SD.remove(MOTION_FILE);
    SD.remove(TRIM_FILE);
    SD.remove(CONFIG_FILE);

    delay(50);

    //--------------------------------------------------
    // モーション保存
    //--------------------------------------------------
    File motionFile = SD.open(MOTION_FILE, FILE_WRITE);

    if (!motionFile) {
        Serial.println("motion save failed");
        return false;
    }

    motionFile.println("PATTERN,STEP,S1,S2,S3,S4,S5,S6");

    for (int p = 0; p < NUM_PATTERNS; p++) {
        for (int s = 0; s < NUM_STEPS; s++) {
            motionFile.printf(
                "%d,%d,%d,%d,%d,%d,%d,%d\n",
                p,
                s,
                motion[p][s][0],
                motion[p][s][1],
                motion[p][s][2],
                motion[p][s][3],
                motion[p][s][4],
                motion[p][s][5]
            );
        }
    }

    motionFile.flush();
    motionFile.close();

    //--------------------------------------------------
    // トリム保存
    //--------------------------------------------------
    File trimFile = SD.open(TRIM_FILE, FILE_WRITE);

    if (!trimFile) {
        Serial.println("trim save failed");
        return false;
    }

    trimFile.println("S1,S2,S3,S4,S5,S6");

    trimFile.printf(
        "%d,%d,%d,%d,%d,%d\n",
        trimAngles[0],
        trimAngles[1],
        trimAngles[2],
        trimAngles[3],
        trimAngles[4],
        trimAngles[5]
    );

    trimFile.flush();
    trimFile.close();

    //--------------------------------------------------
    // 設定保存
    //--------------------------------------------------
    File configFile = SD.open(CONFIG_FILE, FILE_WRITE);

    if (!configFile) {
        Serial.println("config save failed");
        return false;
    }

    configFile.println("PATTERN,PLAY_STEPS");

    for (int p = 0; p < NUM_PATTERNS; p++) {
        configFile.printf("%d,%d\n", p, playSteps[p]);
    }

    configFile.flush();
    configFile.close();

    Serial.println("CSV saved");
    return true;
}

//--------------------------------------------------
// SDカードから読み込み
//--------------------------------------------------
bool loadDataFromSD() {
    if (!sdReady) return false;

    //--------------------------------------------------
    // トリム読み込み
    //--------------------------------------------------
    if (SD.exists(TRIM_FILE)) {
        File trimFile = SD.open(TRIM_FILE, FILE_READ);

        if (trimFile) {
            trimFile.readStringUntil('\n');

            String line = trimFile.readStringUntil('\n');

            int t0, t1, t2, t3, t4, t5;

            int result = sscanf(
                line.c_str(),
                "%d,%d,%d,%d,%d,%d",
                &t0, &t1, &t2, &t3, &t4, &t5
            );

            if (result == 6) {
                trimAngles[0] = constrain(t0, 0, 180);
                trimAngles[1] = constrain(t1, 0, 180);
                trimAngles[2] = constrain(t2, 0, 180);
                trimAngles[3] = constrain(t3, 0, 180);
                trimAngles[4] = constrain(t4, 0, 180);
                trimAngles[5] = constrain(t5, 0, 180);
            }

            trimFile.close();
        }
    }

    //--------------------------------------------------
    // モーション読み込み
    //--------------------------------------------------
    if (SD.exists(MOTION_FILE)) {
        File motionFile = SD.open(MOTION_FILE, FILE_READ);

        if (motionFile) {
            motionFile.readStringUntil('\n');

            while (motionFile.available()) {
                String line = motionFile.readStringUntil('\n');
                line.trim();

                if (line.length() == 0) continue;

                int p, s;
                int v0, v1, v2, v3, v4, v5;

                int result = sscanf(
                    line.c_str(),
                    "%d,%d,%d,%d,%d,%d,%d,%d",
                    &p,
                    &s,
                    &v0, &v1, &v2, &v3, &v4, &v5
                );

                if (result == 8) {
                    if (p >= 0 &&
                        p < NUM_PATTERNS &&
                        s >= 0 &&
                        s < NUM_STEPS) {

                        motion[p][s][0] = constrain(v0, -90, 90);
                        motion[p][s][1] = constrain(v1, -90, 90);
                        motion[p][s][2] = constrain(v2, -90, 90);
                        motion[p][s][3] = constrain(v3, -90, 90);
                        motion[p][s][4] = constrain(v4, -90, 90);
                        motion[p][s][5] = constrain(v5, -90, 90);
                    }
                }
            }

            motionFile.close();
        }
    }

    //--------------------------------------------------
    // 再生ステップ数読み込み
    //--------------------------------------------------
    if (SD.exists(CONFIG_FILE)) {
        File configFile = SD.open(CONFIG_FILE, FILE_READ);

        if (configFile) {
            configFile.readStringUntil('\n');

            while (configFile.available()) {
                String line = configFile.readStringUntil('\n');
                line.trim();

                if (line.length() == 0) continue;

                int p, steps;

                int result = sscanf(
                    line.c_str(),
                    "%d,%d",
                    &p,
                    &steps
                );

                if (result == 2) {
                    if (p >= 0 && p < NUM_PATTERNS) {
                        playSteps[p] = constrain(steps, 1, NUM_STEPS);
                    }
                }
            }

            configFile.close();
        }
    }

    // HOMEは必ず1ステップにする
    playSteps[HOME_PATTERN] = 1;

    Serial.println("CSV loaded");
    return true;
}

//--------------------------------------------------
// BLE接続状態をLEDに反映
//--------------------------------------------------
void updateLED() {
    leds[0] = connected ? CRGB::Green : CRGB::Black;
    FastLED.show();
}

//--------------------------------------------------
// BLE接続確認
//--------------------------------------------------
void checkConnection() {
    if (pClient == nullptr) return;

    bool nowConnected = pClient->isConnected();

    if (connected != nowConnected) {
        connected = nowConnected;

        if (!connected) {
            pRemoteCharacteristic = nullptr;
            playing = false;
        }

        updateLED();
        drawDisplay();
    }
}

//--------------------------------------------------
// サーボ角度をBLEで送信
//
// 送信値 = trimAngles + controlOffsets
//
// Control Mode 1 / 2 のときは、
// サーボ1・2だけランダム顔動作用の値に置き換える
//--------------------------------------------------
void sendAngles() {
    if (!connected ||
        pClient == nullptr ||
        !pClient->isConnected() ||
        pRemoteCharacteristic == nullptr) {
        return;
    }

    uint8_t sendData[NUM_SERVOS];

    for (int i = 0; i < NUM_SERVOS; i++) {
        int offset = controlOffsets[i];

        if ((joyMode == JOY_CONTROL_MODE1 ||
             joyMode == JOY_CONTROL_MODE2) &&
            (i == 0 || i == 1)) {

            offset = randomFaceCurrent[i];
        }

        int value = constrain(
            trimAngles[i] + offset,
            0,
            180
        );

        sendData[i] = value;
    }

    pRemoteCharacteristic->writeValue(
        sendData,
        NUM_SERVOS
    );
}

//--------------------------------------------------
// 現在ステップのオフセット値をモーション配列へ保存
//--------------------------------------------------
void saveStep() {
    for (int i = 0; i < NUM_SERVOS; i++) {
        motion[currentPattern][currentStep][i] =
            controlOffsets[i];
    }
}

//--------------------------------------------------
// 現在ステップのモーションを読み込んで送信
//--------------------------------------------------
void loadCurrentStep() {
    if (currentStep >= playSteps[currentPattern]) {
        currentStep = playSteps[currentPattern] - 1;
    }

    if (currentStep < 0) {
        currentStep = 0;
    }

    for (int i = 0; i < NUM_SERVOS; i++) {
        controlOffsets[i] =
            motion[currentPattern][currentStep][i];
    }

    sendAngles();
}

//--------------------------------------------------
// 次のステップへ
//--------------------------------------------------
void nextStep() {
    currentStep++;

    if (currentStep >= playSteps[currentPattern]) {
        currentStep = 0;
    }

    loadCurrentStep();
    drawDisplay();
}

//--------------------------------------------------
// 前のステップへ
//--------------------------------------------------
void prevStep() {
    currentStep--;

    if (currentStep < 0) {
        currentStep = playSteps[currentPattern] - 1;
    }

    loadCurrentStep();
    drawDisplay();
}

//--------------------------------------------------
// パターン選択
//
// HOME以外は選択した時点で1回再生開始
// HOMEは再生しない
//--------------------------------------------------
void selectPattern(int index) {
    currentPattern = index;
    currentStep = 0;

    loadCurrentStep();

    if (currentPattern == HOME_PATTERN) {
        playing = false;
        currentStep = 0;
    } else {
        currentStep = 1;
        playing = true;
        lastPlayTime = millis();
    }

    drawDisplay();
}

//--------------------------------------------------
// 選択サーボを左へ
//--------------------------------------------------
void selectPrevServo() {
    selectedServo--;

    if (selectedServo < 0) {
        selectedServo = NUM_SERVOS - 1;
    }

    drawDisplay();
}

//--------------------------------------------------
// 選択サーボを右へ
//--------------------------------------------------
void selectNextServo() {
    selectedServo = (selectedServo + 1) % NUM_SERVOS;
    drawDisplay();
}

//--------------------------------------------------
// モーションオフセット変更
//--------------------------------------------------
void changeControl(int index, int delta) {
    if (index < 0 || index >= NUM_SERVOS) return;

    controlOffsets[index] += delta;
    controlOffsets[index] =
        constrain(controlOffsets[index], -90, 90);

    saveStep();
    sendAngles();
    drawDisplay();
}

//--------------------------------------------------
// トリム値変更
//--------------------------------------------------
void changeTrim(int index, int delta) {
    if (index < 0 || index >= NUM_SERVOS) return;

    trimAngles[index] += delta;
    trimAngles[index] =
        constrain(trimAngles[index], 0, 180);

    sendAngles();
    drawDisplay();
}

//--------------------------------------------------
// 現在パターンの再生ステップ数変更
//--------------------------------------------------
void changePlaySteps(int delta) {
    playSteps[currentPattern] += delta;
    playSteps[currentPattern] = constrain(
        playSteps[currentPattern],
        1,
        NUM_STEPS
    );

    if (currentStep >= playSteps[currentPattern]) {
        currentStep = playSteps[currentPattern] - 1;
        loadCurrentStep();
    }

    drawDisplay();
}

//--------------------------------------------------
// サーボ表示ブロック描画
//--------------------------------------------------
void drawServoBlock(
    int x,
    int y,
    const char* label,
    int trim,
    int offset,
    bool selected
) {
    auto& lcd = M5Cardputer.Display;

    uint16_t frameColor = selected ? GREEN : CYAN;

    lcd.drawRoundRect(x, y, 74, 42, 4, frameColor);

    if (selected) {
        lcd.fillRect(x + 2, y + 2, 70, 13, GREEN);
        lcd.setTextColor(BLACK, GREEN);
    } else {
        lcd.setTextColor(WHITE, BLACK);
    }

    lcd.setTextSize(1);
    lcd.setCursor(x + 4, y + 4);
    lcd.printf("%s", label);

    lcd.setCursor(x + 24, y + 4);
    lcd.printf("T:%03d", trim);

    lcd.setTextColor(WHITE, BLACK);

    lcd.setTextSize(2);
    lcd.setCursor(x + 4, y + 20);
    lcd.printf("%+03d", offset);

    int finalAngle = constrain(trim + offset, 0, 180);

    lcd.setTextSize(1);
    lcd.setCursor(x + 45, y + 25);
    lcd.printf("%03d", finalAngle);
}

//--------------------------------------------------
// 保存時のSAVE表示
//--------------------------------------------------
void showSaveMessage() {
    auto& lcd = M5Cardputer.Display;

    int boxW = 100;
    int boxH = 40;

    int x = (240 - boxW) / 2;
    int y = (135 - boxH) / 2;

    lcd.fillRect(x, y, boxW, boxH, RED);
    lcd.drawRect(x, y, boxW, boxH, WHITE);

    lcd.setTextSize(3);
    lcd.setTextColor(WHITE, RED);
    lcd.setCursor(x + 15, y + 9);
    lcd.print("SAVE");

    lcd.setTextSize(1);
}

//--------------------------------------------------
// 画面全体描画
//--------------------------------------------------
void drawDisplay() {
    auto& lcd = M5Cardputer.Display;

    lcd.fillScreen(BLACK);

    //--------------------------------------------------
    // 上部ステータス
    //--------------------------------------------------
    lcd.setTextSize(2);

    lcd.setTextColor(CYAN, BLACK);
    lcd.setCursor(6, 4);
    lcd.printf("%s", patternNames[currentPattern]);

    lcd.setTextColor(WHITE, BLACK);
    lcd.setCursor(70, 4);
    lcd.printf("%02d", currentStep);

    if (trimMode) {
        lcd.fillRect(166, 2, 40, 20, ORANGE);
        lcd.setTextColor(BLACK, ORANGE);
        lcd.setCursor(169, 4);
        lcd.print("TRM");
    } else {
        lcd.setTextColor(LIGHTGREY, BLACK);
        lcd.setCursor(169, 4);
        lcd.print("MOV");
    }

    lcd.setTextColor(GREEN, BLACK);
    lcd.setCursor(212, 4);
    lcd.printf("S%d", selectedServo + 1);

    //--------------------------------------------------
    // サーボブロック
    //--------------------------------------------------
    drawServoBlock(4,   28, "S1", trimAngles[0], controlOffsets[0], selectedServo == 0);
    drawServoBlock(83,  28, "S2", trimAngles[1], controlOffsets[1], selectedServo == 1);
    drawServoBlock(162, 28, "S3", trimAngles[2], controlOffsets[2], selectedServo == 2);

    drawServoBlock(4,   77, "S4", trimAngles[3], controlOffsets[3], selectedServo == 3);
    drawServoBlock(83,  77, "S5", trimAngles[4], controlOffsets[4], selectedServo == 4);
    drawServoBlock(162, 77, "S6", trimAngles[5], controlOffsets[5], selectedServo == 5);

    //--------------------------------------------------
    // 下部ステータス
    //--------------------------------------------------
    lcd.setTextSize(1);

    if (joyMode == JOY_CONTROL_MODE1) {
        lcd.setTextColor(BLUE, BLACK);
        lcd.setCursor(6, 120);
        lcd.print("CTRL:1");
    } else if (joyMode == JOY_CONTROL_MODE2) {
        lcd.setTextColor(YELLOW, BLACK);
        lcd.setCursor(6, 120);
        lcd.print("CTRL:2");
    } else {
        lcd.setTextColor(GREEN, BLACK);
        lcd.setCursor(6, 120);
        lcd.print("JOY:DIRECT");
    }

    lcd.setTextColor(YELLOW, BLACK);
    lcd.setCursor(82, 120);
    lcd.printf("LEN:%02d", playSteps[currentPattern]);
}

//--------------------------------------------------
// BLEサーバーへ接続
//--------------------------------------------------
bool connectToServer() {
    pClient = BLEDevice::createClient();

    if (!pClient->connect(targetAddress)) {
        connected = false;
        updateLED();
        return false;
    }

    BLERemoteService* service =
        pClient->getService(BLEUUID(SERVICE_UUID));

    if (!service) {
        connected = false;
        updateLED();
        return false;
    }

    pRemoteCharacteristic =
        service->getCharacteristic(BLEUUID(CHARACTERISTIC_UUID));

    if (!pRemoteCharacteristic) {
        connected = false;
        updateLED();
        return false;
    }

    connected = true;
    updateLED();

    return true;
}

//--------------------------------------------------
// キー入力処理
//--------------------------------------------------
void handleKey(char c, bool longPress) {
    int delta = longPress ? 10 : 1;

    switch (c) {
        case 'f':
        case 'F':
            selectPattern(0);
            break;

        case 'c':
        case 'C':
            selectPattern(1);
            break;

        case 'v':
        case 'V':
            selectPattern(2);
            break;

        case 'x':
        case 'X':
            selectPattern(3);
            break;

        case 'g':
        case 'G':
            selectPattern(4);
            break;

        case 'd':
        case 'D':
            selectPattern(5);
            break;

        case 'h':
        case 'H':
            selectPattern(HOME_PATTERN);
            break;

        case 't':
        case 'T':
            trimMode = !trimMode;
            drawDisplay();
            break;

        case 's':
        case 'S':
            showSaveMessage();
            saveDataToSD();
            delay(2000);
            drawDisplay();
            break;

        case '[':
            selectPrevServo();
            break;

        case ']':
            selectNextServo();
            break;

        case '<':
            changePlaySteps(-delta);
            break;

        case '>':
            changePlaySteps(delta);
            break;

        case '=':
        case '+':
            trimMode ?
                changeTrim(selectedServo, delta) :
                changeControl(selectedServo, delta);
            break;

        case '-':
        case '_':
            trimMode ?
                changeTrim(selectedServo, -delta) :
                changeControl(selectedServo, -delta);
            break;

        case '1':
            trimMode ? changeTrim(0, delta) : changeControl(0, delta);
            break;

        case 'q':
        case 'Q':
            trimMode ? changeTrim(0, -delta) : changeControl(0, -delta);
            break;

        case '2':
            trimMode ? changeTrim(1, delta) : changeControl(1, delta);
            break;

        case 'w':
        case 'W':
            trimMode ? changeTrim(1, -delta) : changeControl(1, -delta);
            break;

        case '3':
            trimMode ? changeTrim(2, delta) : changeControl(2, delta);
            break;

        case 'e':
        case 'E':
            trimMode ? changeTrim(2, -delta) : changeControl(2, -delta);
            break;

        case '4':
            trimMode ? changeTrim(3, delta) : changeControl(3, delta);
            break;

        case 'r':
        case 'R':
            trimMode ? changeTrim(3, -delta) : changeControl(3, -delta);
            break;

        case '5':
            trimMode ? changeTrim(4, delta) : changeControl(4, delta);
            break;

        case 'y':
        case 'Y':
            trimMode ? changeTrim(4, -delta) : changeControl(4, -delta);
            break;

        case '6':
            trimMode ? changeTrim(5, delta) : changeControl(5, delta);
            break;

        case 'u':
        case 'U':
            trimMode ? changeTrim(5, -delta) : changeControl(5, -delta);
            break;

        case ',':
            prevStep();
            break;

        case '.':
            nextStep();
            break;
    }
}

//--------------------------------------------------
// キーボード状態確認
// 長押し・リピートにも対応
//--------------------------------------------------
void handleKeyboard() {
    auto status = M5Cardputer.Keyboard.keysState();

    bool pressedNow[256] = {false};

    for (auto c : status.word) {
        uint8_t key = (uint8_t)c;
        pressedNow[key] = true;

        unsigned long now = millis();

        if (!keyActive[key]) {
            keyActive[key] = true;
            keyPressStart[key] = now;
            keyLastAction[key] = now;

            handleKey(c, false);
        } else {
            bool longPress =
                now - keyPressStart[key] >= LONG_PRESS_TIME;

            unsigned long repeatTime =
                longPress ?
                LONG_REPEAT_TIME :
                SHORT_REPEAT_TIME;

            if (now - keyLastAction[key] >= repeatTime) {
                keyLastAction[key] = now;
                handleKey(c, longPress);
            }
        }
    }

    for (int i = 0; i < 256; i++) {
        if (keyActive[i] && !pressedNow[i]) {
            keyActive[i] = false;
            keyPressStart[i] = 0;
            keyLastAction[i] = 0;
        }
    }
}

//--------------------------------------------------
// ジョイスティック読み取り
//--------------------------------------------------
bool readJoystick() {
    Wire.beginTransmission(JOY_ADDR);
    Wire.write(0x00);

    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    if (Wire.requestFrom(JOY_ADDR, 4) != 4) {
        return false;
    }

    uint8_t xl = Wire.read();
    uint8_t xh = Wire.read();
    uint8_t yl = Wire.read();
    uint8_t yh = Wire.read();

    int rawX = ((uint16_t)xh << 8) | xl;
    int rawY = ((uint16_t)yh << 8) | yl;

    joyX = rawX - 32768;
    joyY = rawY - 32768;

    //--------------------------------------------------
    // ボタン読み取り
    //--------------------------------------------------
    Wire.beginTransmission(JOY_ADDR);
    Wire.write(0x20);

    if (Wire.endTransmission(false) == 0 &&
        Wire.requestFrom(JOY_ADDR, 1) == 1) {
        joyButton = Wire.read() == 0;
    }

    return true;
}

//--------------------------------------------------
// ジョイスティック中心値キャリブレーション
//--------------------------------------------------
void calibrateJoystick() {
    long sumX = 0;
    long sumY = 0;
    int count = 0;

    for (int i = 0; i < 20; i++) {
        Wire.beginTransmission(JOY_ADDR);
        Wire.write(0x60);

        if (Wire.endTransmission(false) == 0 &&
            Wire.requestFrom(JOY_ADDR, 4) == 4) {

            uint8_t xl = Wire.read();
            uint8_t xh = Wire.read();
            uint8_t yl = Wire.read();
            uint8_t yh = Wire.read();

            int rawX = ((uint16_t)xh << 8) | xl;
            int rawY = ((uint16_t)yh << 8) | yl;

            sumX += rawX;
            sumY += rawY;
            count++;
        }

        delay(10);
    }

    if (count > 0) {
        joyCenterX = sumX / count;
        joyCenterY = sumY / count;
        joyReady = true;
    } else {
        joyReady = false;
    }

    Serial.printf(
        "JOY CENTER X=%d Y=%d READY=%d\n",
        joyCenterX,
        joyCenterY,
        joyReady
    );
}

//--------------------------------------------------
// ジョイスティック値をサーボオフセットへ変換
//--------------------------------------------------
int joyToOffset(int32_t value) {
    if (abs(value) < JOY_DIRECT_DEAD) {
        return 0;
    }

    value = constrain(value, -32768, 32767);

    return map(
        value,
        -32768,
        32767,
        -JOY_SERVO_RANGE,
        JOY_SERVO_RANGE
    );
}

//--------------------------------------------------
// 顔ランダム動作
//
// Control Mode 1 / 2 のときだけ動作
// 1〜3秒ごとにランダム目標値を作り、
// 50msごとに少しずつ近づける
//--------------------------------------------------
void updateRandomFace() {
    if (joyMode != JOY_CONTROL_MODE1 &&
        joyMode != JOY_CONTROL_MODE2) {
        return;
    }

    unsigned long now = millis();

    //--------------------------------------------------
    // 次のランダム目標値を作る
    //--------------------------------------------------
    if (now - lastFaceRandomTime >= nextFaceRandomInterval) {
        lastFaceRandomTime = now;

        nextFaceRandomInterval = random(1000, 3001);

        randomFaceTarget[0] = random(
            -FACE_RANDOM_RANGE,
            FACE_RANDOM_RANGE + 1
        );

        randomFaceTarget[1] = random(
            -FACE_RANDOM_RANGE,
            FACE_RANDOM_RANGE + 1
        );

        Serial.printf(
            "Face X=%d Y=%d Interval=%lu\n",
            randomFaceTarget[0],
            randomFaceTarget[1],
            nextFaceRandomInterval
        );
    }

    //--------------------------------------------------
    // スムーズに現在値を目標値へ近づける
    //--------------------------------------------------
    if (now - lastFaceSmoothTime < FACE_SMOOTH_INTERVAL) {
        return;
    }

    lastFaceSmoothTime = now;

    bool changed = false;

    for (int i = 0; i < 2; i++) {
        if (randomFaceCurrent[i] < randomFaceTarget[i]) {
            randomFaceCurrent[i] += FACE_SMOOTH_STEP;
            changed = true;
        }
        else if (randomFaceCurrent[i] > randomFaceTarget[i]) {
            randomFaceCurrent[i] -= FACE_SMOOTH_STEP;
            changed = true;
        }
    }

    if (changed) {
        sendAngles();
    }
}

//--------------------------------------------------
// ジョイスティック操作
//
// MODE1
//   上:FWD
//   下:BACK
//   右:RTN
//   左:LTN
//
// MODE2
//   上:FWD
//   下:BACK
//   右:RIGHT
//   左:LEFT
//
// DIRECT
//   サーボ1・2を直接操作
//--------------------------------------------------
void handleJoystick() {
    if (!readJoystick()) {
        return;
    }

    unsigned long now = millis();

    //--------------------------------------------------
    // ジョイスティックボタンでモード切替
    //--------------------------------------------------
    if (joyButton && !lastJoyButton) {
        joyMode = (JoyMode)((joyMode + 1) % 3);
        lastJoyAction = now;
        drawDisplay();
    }

    lastJoyButton = joyButton;

    //--------------------------------------------------
    // Control Modeでは連続入力を抑制
    //--------------------------------------------------
    if (joyMode != JOY_DIRECT_MODE) {
        if (now - lastJoyAction < JOY_REPEAT_TIME) {
            return;
        }
    }

    int32_t absX = abs(joyX);
    int32_t absY = abs(joyY);

    if (joyMode != JOY_DIRECT_MODE) {
        lastJoyAction = now;
    }

    //--------------------------------------------------
    // Control Mode 1
    //--------------------------------------------------
    if (joyMode == JOY_CONTROL_MODE1) {
        if (absX < JOY_CURSOR_DEAD &&
            absY < JOY_CURSOR_DEAD) {
            return;
        }

        if (absY > absX) {
            if (joyY < -JOY_CURSOR_DEAD) {
                selectPattern(0);   // FWD
            } else if (joyY > JOY_CURSOR_DEAD) {
                selectPattern(1);   // BACK
            }
        } else {
            if (joyX > JOY_CURSOR_DEAD) {
                selectPattern(4);   // RTN
            } else if (joyX < -JOY_CURSOR_DEAD) {
                selectPattern(5);   // LTN
            }
        }

        return;
    }

    //--------------------------------------------------
    // Control Mode 2
    //--------------------------------------------------
    if (joyMode == JOY_CONTROL_MODE2) {
        if (absX < JOY_CURSOR_DEAD &&
            absY < JOY_CURSOR_DEAD) {
            return;
        }

        if (absY > absX) {
            if (joyY < -JOY_CURSOR_DEAD) {
                selectPattern(0);   // FWD
            } else if (joyY > JOY_CURSOR_DEAD) {
                selectPattern(1);   // BACK
            }
        } else {
            if (joyX > JOY_CURSOR_DEAD) {
                selectPattern(2);   // RIGHT
            } else if (joyX < -JOY_CURSOR_DEAD) {
                selectPattern(3);   // LEFT
            }
        }

        return;
    }

    //--------------------------------------------------
    // Direct Mode
    //--------------------------------------------------
    if (joyMode == JOY_DIRECT_MODE) {
        controlOffsets[0] = joyToOffset(-joyX);
        controlOffsets[1] = joyToOffset(joyY);

        saveStep();
        sendAngles();
        drawDisplay();

        return;
    }
}

//--------------------------------------------------
// 初期化
//--------------------------------------------------
void setup() {
    auto cfg = M5.config();

    //--------------------------------------------------
    // Cardputer初期化
    //--------------------------------------------------
    M5Cardputer.begin(cfg);

    Serial.begin(115200);

    //--------------------------------------------------
    // ランダム初期化
    //--------------------------------------------------
    randomSeed(micros());
    nextFaceRandomInterval = random(1000, 3001);

    //--------------------------------------------------
    // I2Cジョイスティック初期化
    //--------------------------------------------------
    Wire.begin(JOY_SDA_PIN, JOY_SCL_PIN);
    calibrateJoystick();

    //--------------------------------------------------
    // BLE初期化
    //--------------------------------------------------
    BLEDevice::init("M5Cardputer_Sender");

    //--------------------------------------------------
    // LED初期化
    //--------------------------------------------------
    FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, NUM_LEDS);
    FastLED.show();

    //--------------------------------------------------
    // モーション配列初期化
    //--------------------------------------------------
    for (int p = 0; p < NUM_PATTERNS; p++) {
        for (int s = 0; s < NUM_STEPS; s++) {
            for (int i = 0; i < NUM_SERVOS; i++) {
                motion[p][s][i] = 0;
            }
        }
    }

    //--------------------------------------------------
    // HOMEは必ず全サーボ0オフセット
    //--------------------------------------------------
    for (int i = 0; i < NUM_SERVOS; i++) {
        motion[HOME_PATTERN][0][i] = 0;
    }

    //--------------------------------------------------
    // SDカード初期化と読み込み
    //--------------------------------------------------
    sdReady = initSD();

    if (sdReady) {
        loadDataFromSD();
    }

    //--------------------------------------------------
    // SD読み込み後もHOMEを保証
    //--------------------------------------------------
    for (int i = 0; i < NUM_SERVOS; i++) {
        motion[HOME_PATTERN][0][i] = 0;
    }

    //--------------------------------------------------
    // 起動時はHOMEから開始
    //--------------------------------------------------
    currentPattern = HOME_PATTERN;
    currentStep = 0;
    loadCurrentStep();

    drawDisplay();

    //--------------------------------------------------
    // BLE接続
    //--------------------------------------------------
    connected = connectToServer();

    updateLED();

    //--------------------------------------------------
    // 接続後にHOME角度を送信
    //--------------------------------------------------
    sendAngles();

    drawDisplay();
}

//--------------------------------------------------
// メインループ
//
// 1. Cardputer更新
// 2. BLE接続確認
// 3. キーボード処理
// 4. ジョイスティック処理
// 5. 顔ランダム処理
// 6. モーション再生処理
//--------------------------------------------------
void loop() {
    M5Cardputer.update();

    //--------------------------------------------------
    // BLE接続状態確認
    //--------------------------------------------------
    checkConnection();

    //--------------------------------------------------
    // キーボード処理
    //--------------------------------------------------
    if (M5Cardputer.Keyboard.isPressed()) {
        handleKeyboard();
    } else {
        for (int i = 0; i < 256; i++) {
            keyActive[i] = false;
            keyPressStart[i] = 0;
            keyLastAction[i] = 0;
        }
    }

    //--------------------------------------------------
    // ジョイスティック処理
    //--------------------------------------------------
    handleJoystick();

    //--------------------------------------------------
    // Control Mode時の顔ランダム動作
    //--------------------------------------------------
    updateRandomFace();

    //--------------------------------------------------
    // モーション1回再生処理
    //--------------------------------------------------
    if (playing &&
        millis() - lastPlayTime >= 100) {

        loadCurrentStep();

        currentStep++;

        if (currentStep >= playSteps[currentPattern]) {
            playing = false;
            currentStep = 0;
            drawDisplay();
        } else {
            drawDisplay();
        }

        lastPlayTime = millis();
    }

    delay(1);
}