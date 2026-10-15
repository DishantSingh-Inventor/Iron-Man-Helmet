#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <driver/i2s.h>
#include <math.h>

const int LED_LEFT_PIN = 11;
const int LED_RIGHT_PIN = 12;
const int SERVO_LEFT_PIN = 8;
const int SERVO_RIGHT_PIN = 9;
const int TOUCH_PIN = 10;
const int I2S_BCLK_PIN = 4;
const int I2S_WS_PIN = 5;
const int I2S_MIC_PIN = 6;
const int I2S_AMP_PIN = 7;

const int SERVO_CLOSED_LEFT = 0;
const int SERVO_CLOSED_RIGHT = 0;
const int SERVO_OPEN_LEFT = 90;
const int SERVO_OPEN_RIGHT = 90;

const char *AP_NAME = "Classified";
const char *AP_PASSWORD = "LadisMan217";

const uint32_t AUDIO_RATE = 16000;
const size_t AUDIO_FRAMES = 128;

WebServer server(80);
Servo servoLeft;
Servo servoRight;
volatile bool audioEnabled = false;
volatile bool lightsEnabled = true;
volatile bool helmetOpen = false;
volatile float modulationHz = 70.0f;
volatile float audioLevel = 0.75f;
volatile bool audioReady = false;
volatile bool touchEnabled = true;
float phase = 0.0f;
bool previousTouch = false;

const char page[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Classified</title>
<style>
body{font-family:Arial,sans-serif;max-width:520px;margin:32px auto;padding:0 16px;background:#101317;color:#eef2f6}
h1{font-size:25px;letter-spacing:1px}.card{background:#1b2027;border:1px solid #303844;border-radius:12px;padding:16px;margin:14px 0}
button{border:0;border-radius:8px;padding:12px 14px;margin:5px 4px 5px 0;background:#2589dc;color:white;font-size:15px;cursor:pointer}
button.secondary{background:#394351}button.danger{background:#b84444}
label{display:block;margin:14px 0 8px}input{width:100%;accent-color:#2589dc}
small,#status{color:#aab5c3}#status{white-space:pre-wrap}
</style>
</head>
<body>
<h1>HELMET CONTROL</h1>
<p>control panel</p>
<div class="card">
<h3>Lights</h3>
<button onclick="send('/lights?on=1')">Lights on</button>
<button class="secondary" onclick="send('/lights?on=0')">Lights off</button>
</div>
<div class="card">
<h3>Servos</h3>
<button onclick="send('/helmet?open=1')">Open</button>
<button class="secondary" onclick="send('/helmet?open=0')">Close</button>
</div>
<div class="card">
<h3>Voice effect</h3>
<button onclick="send('/audio?on=1')">Audio on</button>
<button class="danger" onclick="send('/audio?on=0')">Audio off</button>
<label for="freq">Robotic modulation: <span id="freqValue">70</span> Hz</label>
<input id="freq" type="range" min="25" max="140" value="70" oninput="freqValue.textContent=this.value" onchange="send('/audio-settings?freq='+this.value)">
<label for="level">Output level: <span id="levelValue">75</span>%</label>
<input id="level" type="range" min="10" max="100" value="75" oninput="levelValue.textContent=this.value" onchange="send('/audio-settings?level='+this.value)">
<p><small>Use a low speaker volume during first tests.</small></p>
</div>
<div class="card"><h3>Status</h3><div id="status">Connecting...</div><button class="secondary" onclick="refreshStatus()">Refresh</button></div>
<script>
async function send(path){try{const r=await fetch(path);document.getElementById('status').textContent=await r.text();refreshStatus()}catch(e){document.getElementById('status').textContent='Connection failed'}}
async function refreshStatus(){try{const r=await fetch('/status');const s=await r.json();document.getElementById('status').textContent='Lights: '+(s.lights?'ON':'OFF')+'\nHelmet: '+(s.open?'OPEN':'CLOSED')+'\nAudio: '+(s.audio?'ON':'OFF')+'\nI2S: '+(s.i2s?'READY':'NOT READY')}catch(e){document.getElementById('status').textContent='Connection failed'}}
refreshStatus();
</script>
</body>
</html>
)HTML";

void setLights(bool enabled) {
  lightsEnabled = enabled;
  digitalWrite(LED_LEFT_PIN, enabled ? HIGH : LOW);
  digitalWrite(LED_RIGHT_PIN, enabled ? HIGH : LOW);
}

void setHelmet(bool open) {
  helmetOpen = open;
  servoLeft.write(open ? SERVO_OPEN_LEFT : SERVO_CLOSED_LEFT);
  servoRight.write(open ? SERVO_OPEN_RIGHT : SERVO_CLOSED_RIGHT);
}

void sendText(const String &message, int code = 200) {
  server.send(code, "text/plain", message);
}

void handleRoot() {
  server.send_P(200, "text/html", page);
}

void handleLights() {
  setLights(server.arg("on") == "1");
  sendText(lightsEnabled ? "Lights on" : "Lights off");
}

void handleHelmet() {
  setHelmet(server.arg("open") == "1");
  sendText(helmetOpen ? "Helmet opening position set" : "Helmet closing position set");
}

void handleAudio() {
  audioEnabled = server.arg("on") == "1";
  sendText(audioEnabled ? "Voice processing enabled" : "Voice processing disabled");
}

void handleAudioSettings() {
  if (server.hasArg("freq")) {
    modulationHz = constrain(server.arg("freq").toFloat(), 25.0f, 140.0f);
  }
  if (server.hasArg("level")) {
    audioLevel = constrain(server.arg("level").toFloat() / 100.0f, 0.1f, 1.0f);
  }
  sendText("Audio settings updated");
}

void handleStatus() {
  String result = "{\"lights\":";
  result += lightsEnabled ? "true" : "false";
  result += ",\"open\":";
  result += helmetOpen ? "true" : "false";
  result += ",\"audio\":";
  result += audioEnabled ? "true" : "false";
  result += ",\"i2s\":";
  result += audioReady ? "true" : "false";
  result += "}";
  server.send(200, "application/json", result);
}

bool setupAudio() {
  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  config.sample_rate = AUDIO_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = AUDIO_FRAMES;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;

  if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK) {
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.bck_io_num = I2S_BCLK_PIN;
  pins.ws_io_num = I2S_WS_PIN;
  pins.data_out_num = I2S_AMP_PIN;
  pins.data_in_num = I2S_MIC_PIN;
  pins.mck_io_num = I2S_PIN_NO_CHANGE;

  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  return true;
}

void audioTask(void *parameter) {
  int32_t input[AUDIO_FRAMES * 2];
  int32_t output[AUDIO_FRAMES * 2];
  size_t bytesRead = 0;
  size_t bytesWritten = 0;

  for (;;) {
    esp_err_t result = i2s_read(I2S_NUM_0, input, sizeof(input), &bytesRead, portMAX_DELAY);
    if (result != ESP_OK || bytesRead == 0) {
      vTaskDelay(1);
      continue;
    }

    size_t samples = bytesRead / sizeof(int32_t);
    float currentFrequency = modulationHz;
    float currentLevel = audioLevel;
    bool enabled = audioEnabled;

    for (size_t i = 0; i + 1 < samples; i += 2) {
      int32_t source = input[i] >> 16;
      int32_t processed = 0;

      if (enabled) {
        float carrier = sinf(phase);
        float value = (float)source * carrier * currentLevel;
        processed = (int32_t)constrain(value, -32768.0f, 32767.0f);
      }

      output[i] = processed << 16;
      output[i + 1] = processed << 16;
      phase += 2.0f * PI * currentFrequency / AUDIO_RATE;
      if (phase >= 2.0f * PI) {
        phase -= 2.0f * PI;
      }
    }

    i2s_write(I2S_NUM_0, output, samples * sizeof(int32_t), &bytesWritten, portMAX_DELAY);
  }
}

void setup() {
  pinMode(LED_LEFT_PIN, OUTPUT);
  pinMode(LED_RIGHT_PIN, OUTPUT);
  pinMode(TOUCH_PIN, INPUT_PULLDOWN);
  setLights(true);

  servoLeft.setPeriodHertz(50);
  servoRight.setPeriodHertz(50);
  servoLeft.attach(SERVO_LEFT_PIN, 500, 2400);
  servoRight.attach(SERVO_RIGHT_PIN, 500, 2400);
  setHelmet(false);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_NAME, AP_PASSWORD);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/lights", HTTP_GET, handleLights);
  server.on("/helmet", HTTP_GET, handleHelmet);
  server.on("/audio", HTTP_GET, handleAudio);
  server.on("/audio-settings", HTTP_GET, handleAudioSettings);
  server.on("/status", HTTP_GET, handleStatus);
  server.begin();

  audioReady = setupAudio();
  if (audioReady) {
    xTaskCreatePinnedToCore(audioTask, "audioTask", 8192, nullptr, 2, nullptr, 0);
  }
}

void loop() {
  server.handleClient();

  bool touchNow = digitalRead(TOUCH_PIN) == HIGH;
  if (touchEnabled && touchNow && !previousTouch) {
    setHelmet(true);
  }
  previousTouch = touchNow;

  delay(2);
}



