#include <Arduino.h>
#include <Adafruit_SSD1306.h> // 外付けOLED(SSD1306)を使うためのライブラリ。別途「Adafruit BusIO」も必要
#include <M5Unified.h>        // M5AtomS3R本体のLCD、ボタンなどを使うためのライブラリ
#include <Wire.h>             // I2C通信用ライブラリ。OLEDとの通信に使用
#include <BLEDevice.h>        // BLE機能を使うためのライブラリ
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// =========================
// BLE設定
// =========================
// Cardputerなどのコントローラー側と通信するためのUUID
// コントローラー側も同じUUIDにしておく必要がある
#define SERVICE_UUID "1010"
#define CHARACTERISTIC_UUID "1012"

// =========================
// サーボ数
// =========================
// このロボットでは6個のサーボを制御する
#define NUM_SERVOS 6

// BLEサーバーとCharacteristicのポインタ
BLEServer* pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;

// BLE接続中かどうかを示すフラグ
bool deviceConnected = false;

// =========================
// OLED設定
// =========================
#define SCREEN_WIDTH 128   // OLEDの横幅
#define SCREEN_HEIGHT 64   // OLEDの縦幅
#define OLED_RESET -1      // OLEDリセット端子。未使用なので-1

// =========================
// AtomS3R内蔵LCD用スプライト
// =========================
// 直接LCDに描画するとちらつきやすいので、スプライトに描いてから一括表示する
M5Canvas lcdSprite(&M5.Display);

// =========================
// サーボ設定
// =========================
// 各サーボを接続しているGPIOピン
const int servoPins[NUM_SERVOS] = {5, 6, 7, 8, 38, 39};

// ESP32のPWMチャンネル。サーボごとに別チャンネルを使う
const int servoCH[NUM_SERVOS]   = {0, 1, 2, 3, 4, 5};

// サーボ用PWM設定
const int PWM_Hz = 50;             // 一般的なサーボは50Hz
const int PWM_RESOLUTION = 14;     // PWM分解能

// サーボ角度0度、180度に対応するパルス幅相当の値
// サーボの種類によって微調整が必要な場合がある
const int pulseMIN = 410;
const int pulseMAX = 2048;

// サーボを滑らかに動かすための分割数と待ち時間
// 10分割 × 5ms = 約50msで目標角度へ移動
const int INTERP_STEPS = 10;
const int INTERP_DELAY_MS = 5;

// =========================
// OLED顔表示用パラメータ
// =========================
int Sw = SCREEN_WIDTH;
int Sh = SCREEN_HEIGHT;

// 左右の目の間隔
int Pe = 60;

// 目の幅と高さ
int We = 20;
int He = 26;

// まばたき時の細い目の高さ
int Hec = 8;

// 右目・左目の基準X位置
int ReX = Sw/2 - Pe/2 - We/2;
int LeX = Sw/2 + Pe/2 - We/2;

// 通常目の基準Y位置
int eY = Sh/2 - He/2;

// まばたき目の基準Y位置
int ecY = Sh/2 - Hec/2;

// 目の角丸半径
int Re = 6;

// 未使用の変数。将来の表情切替などに使える
int eC = 0;

// サーボ1、サーボ2の角度に応じて目線を動かすためのオフセット
int eyeOffsetX = 0;
int eyeOffsetY = 0;

// I2Cに接続されたSSD1306用displayオブジェクト
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// =========================
// サーボ角度管理
// =========================
// 現在出力している角度。初期値は全て90度
int angles[NUM_SERVOS] = {
    90, 90, 90, 90, 90, 90
};

// 滑らか移動処理で現在位置として使う角度
int currentAngles[NUM_SERVOS] = {
    90, 90, 90, 90, 90, 90
};

// BLE受信後、サーボ移動要求があるかどうか
volatile bool motionRequest = false;

// サーボが移動処理中かどうか
volatile bool motionBusy = false;

// BLEで受信した目標角度を一時保存する配列
int requestedAngles[NUM_SERVOS] = {
    90, 90, 90, 90, 90, 90
};

// =========================
// 顔表示
// =========================

// OLED画面をクリアする
void face_clear(){
  display.clearDisplay();
}

// 通常の丸四角い目を描画する
void face(){
  face_clear();

  // 右目を描画
  display.fillRoundRect(
    ReX + eyeOffsetX,
    eY  + eyeOffsetY,
    We, He, Re,
    SSD1306_WHITE
  );

  // 左目を描画
  display.fillRoundRect(
    LeX + eyeOffsetX,
    eY  + eyeOffsetY,
    We, He, Re,
    SSD1306_WHITE
  );

  // OLEDへ反映
  display.display();
}

// うれしい顔用の片目を描画する
// 円を複数重ねて、弧のような笑い目を作っている
void drawHappyEye(int x, int y)
{
    display.fillCircle(x +  8, y + 16, 4, SSD1306_WHITE);
    display.fillCircle(x + 16, y +  8, 4, SSD1306_WHITE);
    display.fillCircle(x + 24, y + 16, 4, SSD1306_WHITE);

    display.fillCircle(x + 12, y + 12, 4, SSD1306_WHITE);
    display.fillCircle(x + 20, y + 12, 4, SSD1306_WHITE);
}

// うれしい顔を描画する
void happyFace()
{
    face_clear();

    // 顔の上下位置
    int y = Sh / 2 - 8;

    // 右目と左目を描画
    drawHappyEye(ReX + eyeOffsetX - 8, y - 8);
    drawHappyEye(LeX + eyeOffsetX - 8, y - 8);

    display.display();
}

// OLEDの顔表示タスク
// 通常顔、まばたき、うれしい顔を繰り返す
void face_center_eye(void *pvParameters){
  int count = 0;

  while(1)
  {
    // 約5回に1回、うれしい顔を表示
    if (count >= 5) {
      happyFace();
      delay(1000);
      count = 0;
    }

    // 通常の目を表示
    face();
    delay(1000);

    // まばたき用の細い目を表示
    face_clear();

    // 右目のまばたき線
    display.fillRect(
        ReX + eyeOffsetX,
        ecY + eyeOffsetY,
        We, Hec,
        SSD1306_WHITE
    );

    // 左目のまばたき線
    display.fillRect(
        LeX + eyeOffsetX,
        ecY + eyeOffsetY,
        We, Hec,
        SSD1306_WHITE
    );

    display.display();
    delay(200);

    count++;
  }
}

// =========================
// 関数宣言
// =========================
void setupBLE();
void servoWriteAngle(int ch, int angle);
void servoWriteAnglesSmooth(int newAngles[NUM_SERVOS]);

// =========================
// サーボ制御
// =========================

// 指定したPWMチャンネルのサーボを指定角度へ動かす
void servoWriteAngle(int ch, int angle) {
    // 安全のため角度を0〜180度に制限
    angle = constrain(angle, 0, 180);

    // 角度をPWM duty値へ変換
    int duty = map(
        angle,
        0,
        180,
        pulseMIN,
        pulseMAX
    );

    // PWM出力
    ledcWrite(ch, duty);
}

// 複数サーボを現在角度から目標角度へ滑らかに動かす
void servoWriteAnglesSmooth(int newAngles[NUM_SERVOS]) {
    int startAngles[NUM_SERVOS];
    int targetAngles[NUM_SERVOS];

    // 現在角度と目標角度を準備
    for (int i = 0; i < NUM_SERVOS; i++) {
        startAngles[i] = currentAngles[i];
        targetAngles[i] = constrain(newAngles[i], 0, 180);
    }

    // INTERP_STEPS回に分けて少しずつ角度を変える
    for (int step = 1; step <= INTERP_STEPS; step++) {
        for (int i = 0; i < NUM_SERVOS; i++) {
            int angle =
                startAngles[i] +
                (targetAngles[i] - startAngles[i]) * step / INTERP_STEPS;

            servoWriteAngle(servoCH[i], angle);
            angles[i] = angle;
        }

        delay(INTERP_DELAY_MS);
    }

    // 最終角度を現在角度として記録
    for (int i = 0; i < NUM_SERVOS; i++) {
        currentAngles[i] = targetAngles[i];
        angles[i] = targetAngles[i];
    }

    // =========================
    // サーボ角度から目線位置を計算
    // =========================
    // Servo1 : 左右
    // ±20度 → OLED上では±10ドット
    // 符号をマイナスにしているため、サーボの向きと目線の向きを反転している
    eyeOffsetX = - (currentAngles[0] - 90) * 10 / 20;

    // Servo2 : 上下
    // ±20度 → OLED上では±10ドット
    eyeOffsetY = (currentAngles[1] - 90) * 10 / 20;

    // 上方向だけ行き過ぎないように最大2ドットに制限
    // OLED座標では上方向がマイナスになる
    if (eyeOffsetY < -2) {
        eyeOffsetY = -2;
    }
}

// =========================
// AtomS3R内蔵LCD ハート表示
// =========================

// ハート形状をスプライトへ描画する
void drawHeartSprite(M5Canvas &spr, int cx, int cy, float scale)
{
    const int N = 60;  // ハート輪郭の点数。多いほど滑らか
    int px[N];
    int py[N];

    // 数式でハート形状の輪郭点を作る
    for (int i = 0; i < N; i++) {
        float t = TWO_PI * i / N;

        float x = 16 * pow(sin(t), 3);
        float y =
            13 * cos(t)
            - 5 * cos(2 * t)
            - 2 * cos(3 * t)
            - cos(4 * t);

        px[i] = cx + (int)(x * scale);
        py[i] = cy - (int)(y * scale);
    }

    // 輪郭点を三角形で塗りつぶしてハートを描画
    for (int i = 1; i < N - 1; i++) {
        spr.fillTriangle(
            px[0], py[0],
            px[i], py[i],
            px[i + 1], py[i + 1],
            RED
        );
    }
}

// 内蔵LCDにゆっくり点滅するハートを表示するタスク
void heartTask(void *pvParameters)
{
    const int centerX = M5.Display.width() / 2;
    const int centerY = M5.Display.height() / 2 - 8;

    while (1)
    {
        // ハートを大きくしながら明るくする
        for (float s = 2.2f; s <= 2.8f; s += 0.04f)
        {
            int brightness = map((int)(s * 100), 220, 280, 40, 255);

            M5.Display.setBrightness(brightness);

            lcdSprite.fillScreen(BLACK);
            drawHeartSprite(lcdSprite, centerX, centerY, s);
            lcdSprite.pushSprite(0, 0);

            vTaskDelay(25 / portTICK_PERIOD_MS);
        }

        // ハートを小さくしながら暗くする
        for (float s = 2.8f; s >= 2.2f; s -= 0.04f)
        {
            int brightness = map((int)(s * 100), 220, 280, 40, 255);

            M5.Display.setBrightness(brightness);

            lcdSprite.fillScreen(BLACK);
            drawHeartSprite(lcdSprite, centerX, centerY, s);
            lcdSprite.pushSprite(0, 0);

            vTaskDelay(100 / portTICK_PERIOD_MS);
        }

        vTaskDelay(120 / portTICK_PERIOD_MS);
    }
}

// =========================
// BLE受信
// =========================

// コントローラー側から角度データを書き込まれた時に呼ばれる処理
class MyCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pChar) override {
        std::string value = pChar->getValue();

        // 6個分のサーボ角度データが届いているか確認
        if (value.length() >= NUM_SERVOS) {
            for (int i = 0; i < NUM_SERVOS; i++) {
                // 1バイトを0〜180度の角度として受け取る
                requestedAngles[i] = (uint8_t)value[i];
            }

            // loop側へ「新しい角度へ動かして」と通知
            motionRequest = true;
        }
    }
};

// =========================
// BLE接続状態
// =========================

// BLE接続、切断時の処理
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        deviceConnected = true;
        Serial.println("BLE Connected");
    }

    void onDisconnect(BLEServer* pServer) override {
        deviceConnected = false;
        Serial.println("BLE Disconnected");

        // 切断後も再接続できるようにAdvertisingを再開
        BLEDevice::startAdvertising();
    }
};

// =========================
// BLE初期化
// =========================
void setupBLE() {
    // BLEデバイス名。スマホやCardputer側から見える名前
    BLEDevice::init("M5AtomS3R_SERVO");

    // BLEサーバー作成
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    // BLEサービス作成
    BLEService* pService =
        pServer->createService(SERVICE_UUID);

    // 書き込み用Characteristicを作成
    pCharacteristic =
        pService->createCharacteristic(
            CHARACTERISTIC_UUID,
            BLECharacteristic::PROPERTY_WRITE
        );

    // 書き込み時のコールバックを登録
    pCharacteristic->setCallbacks(new MyCallbacks());

    // サービス開始
    pService->start();

    // Advertising設定
    BLEAdvertising* pAdvertising =
        BLEDevice::getAdvertising();

    pAdvertising->addServiceUUID(SERVICE_UUID);

    // BLE接続待ち開始
    BLEDevice::startAdvertising();

    Serial.println("BLE advertising started");
}

// =========================
// setup
// =========================
void setup() {
    // M5AtomS3R初期化
    auto cfg = M5.config();
    M5.begin(cfg);

    // 内蔵LCD用スプライト作成
    lcdSprite.createSprite(M5.Display.width(), M5.Display.height());
    lcdSprite.setTextColor(WHITE);
    lcdSprite.setTextFont(2);

    // シリアル通信開始
    Serial.begin(115200);

    // 内蔵LCDの初期表示設定
    M5.Display.setBrightness(128);
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(WHITE);
    M5.Display.setRotation(2);
    M5.Display.setTextFont(2);

    // OLED用I2Cピン設定
    // SDA=1, SCL=2
    Wire.begin(1, 2);

    // OLED初期設定
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        M5.Display.println("OLED NG");
        return;
    }
    M5.Display.println("OLED OK");

    // OLEDの文字色設定
    display.setTextColor(SSD1306_WHITE);

    // サーボ用PWM初期化
    for (int i = 0; i < NUM_SERVOS; i++) {
        ledcSetup(
            servoCH[i],
            PWM_Hz,
            PWM_RESOLUTION
        );

        ledcAttachPin(
            servoPins[i],
            servoCH[i]
        );

        // 起動時は全サーボ90度にする
        servoWriteAngle(
            servoCH[i],
            angles[i]
        );
    }

    // BLE開始
    setupBLE();

    // OLED顔表示タスク開始
    xTaskCreatePinnedToCore(face_center_eye, "face_center_eye", 4096, NULL, 1, NULL, 1);

    // 内蔵LCDハート表示タスク開始
    xTaskCreatePinnedToCore(heartTask, "heartTask", 4096, NULL, 2, NULL, 1);
}

// =========================
// loop
// =========================
void loop() {
    // M5ボタン状態更新
    M5.update();

    // BLEで新しい角度を受信していて、サーボが動作中でなければ処理する
    if (motionRequest && !motionBusy) {
        motionRequest = false;
        motionBusy = true;

        int workAngles[NUM_SERVOS];

        // 割り込みに近いBLE受信側の配列を直接使わず、作業用配列へコピー
        for (int i = 0; i < NUM_SERVOS; i++) {
            workAngles[i] = requestedAngles[i];
        }

        // サーボを滑らかに目標角度へ移動
        servoWriteAnglesSmooth(workAngles);

        motionBusy = false;
    }

    // BtnAで全サーボ90度リセット
    if (M5.BtnA.wasPressed()) {
        int resetAngles[NUM_SERVOS] = {
            90, 90, 90, 90, 90, 90
        };

        servoWriteAnglesSmooth(resetAngles);
    }

    // CPUを占有しすぎないように短く待つ
    delay(1);
}
