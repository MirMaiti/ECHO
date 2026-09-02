# ECHO - Edge Conversational Hospitality Operator

An enterprise-grade, multi-tenant Cloud AI voice assistant hardware and software system powered by the **ESP32-S3**, **OpenAI (Whisper, GPT-4o-mini, TTS)**, and a **Python Async WebSocket Server**.

Designed for commercial and luxury environments, the system uses the ESP32-S3's factory-burned **eFuse MAC address** to securely identify each physical device. It dynamically fetches customer-specific database context (e.g., room details, guest preferences, local Wi-Fi passwords), streams bidirectional raw PCM audio with sub-second latency, and features local synthesized chimes alongside an OLED interface for facial expressions.

---

## 🛠️ Hardware Requirements & Pinout

### Component List

* **Microcontroller:** ESP32-S3 Development Board (Dual I2S support)
* **Audio Amplifier:** MAX98357A I2S 3W Class-D Breakout Board
* **Microphone:** INMP441 / ICS43434 Omnidirectional MEMS Microphone (Top-port recommended for commercial assembly)
* **Speaker:** 3W 4-Ohm Enclosed Speaker
* **Display:** 0.96" I2C OLED Display (SSD1306)
* **Controls:** 2x Momentary Push Buttons (1x Push-to-Talk, 1x Wi-Fi Reset)
* **Power:** 4.5V Battery Pack (3x AA cells) for maximum safe hardware volume

---

### Master Wiring Table

#### 1. Speaker Amplifier (MAX98357A) – I2S Port 0 (TX)

*Note: Configured for hardware gain (12dB) with a safe digital multiplier (2.0x) in software.*

| MAX98357A Pin | ESP32-S3 Pin | Function | Notes |
| --- | --- | --- | --- |
| **VIN** | **4.5V / 5V** | Power | Connect directly to positive terminal of 4.5V battery pack |
| **GND** | GND | Ground | Common Ground |
| **GAIN** | **GND** | Hardware Gain | Tied to GND for 12dB volume |
| **SD** | **GPIO 18** | Enable Pin | Controlled by ESP32 to wake/sleep amplifier |
| **BCLK** | **GPIO 5** | Bit Clock | Serial Clock |
| **LRC** | **GPIO 6** | Left/Right Clock | Word Select |
| **DIN** | **GPIO 7** | Data In | Audio Data Out from ESP32 |

#### 2. Microphone (INMP441 / ICS43434) – I2S Port 1 (RX)

| Mic Pin | ESP32-S3 Pin | Function | Notes |
| --- | --- | --- | --- |
| **VDD** | 3.3V | Power | Regulated 3.3V from ESP32 |
| **GND** | GND | Ground | Common Ground |
| **L/R** | GND | Channel Select | Tied LOW for Left channel output |
| **SCK** | **GPIO 12** | Bit Clock | Serial Clock |
| **WS** | **GPIO 13** | Word Select | Left/Right Clock |
| **SD** | **GPIO 14** | Serial Data | Audio Data In to ESP32 |

#### 3. OLED Display (SSD1306) – I2C

| OLED Pin | ESP32-S3 Pin | Function | Notes |
| --- | --- | --- | --- |
| **VCC** | 3.3V | Power | **Must be 3.3V**, do not use 4.5V/5V |
| **GND** | GND | Ground | Common Ground |
| **SCL** | **GPIO 9** | I2C Clock | Explicitly defined in code to bypass default pins |
| **SDA** | **GPIO 8** | I2C Data | Explicitly defined in code |

#### 4. Control Buttons

| Control | ESP32-S3 Pin | Configuration | Notes |
| --- | --- | --- | --- |
| **Push-To-Talk (PTT)** | **GPIO 15** | `INPUT_PULLUP` | Wire button between GPIO 15 & GND |
| **Wi-Fi Reset** | **GPIO 4** | `INPUT_PULLUP` | Wire button between GPIO 4 & GND (Hold 2s to reset) |

---

## 📦 Software & Tool Requirements

### 1. ESP32-S3 Firmware (C++)

#### Development Environment

* **IDE:** [Arduino IDE 2.x](https://www.arduino.cc/en/software)
* **Board Selection:** `ESP32S3 Dev Module`

#### Arduino C++ Libraries (Install via Library Manager)

* **`WiFiManager`** (by *tzapu*): Handles captive portal Wi-Fi provisioning.
* **`WebSocketsClient`** (by *Markus Sattler*): Asynchronous WebSocket connection supporting `wss://`.
* **`Adafruit GFX Library`** & **`Adafruit SSD1306`**: For rendering facial expressions on the OLED.
* **`driver/i2s.h`**: Built-in Espressif SDK driver.

---

### 2. Python Backend Server (`server.py`)

#### System Requirements

* **Runtime:** Python 3.10+
* **Database:** PostgreSQL 13+ (Master Device Registry)
* **OS:** Ubuntu 22.04 LTS (Recommended for Cloud VPS)

#### Python Package Dependencies

```bash
pip install websockets openai asyncpg httpx python-dotenv

```

---

## 🔄 End-to-End System Data Flow

```
┌────────────────────────┐                   ┌───────────────────────────────────┐
│  ESP32-S3 Hardware     │                   │  Python Async WebSocket Server    │
│  (Displays IDLE Face)  │                   │  (Hosted on Cloud VPS)            │
└───────────┬────────────┘                   └─────────────────┬─────────────────┘
            │                                                  │
            │  1. Hardware Boot (Plays Sine-Wave Boot Chime)   │
            │  2. WSS Connect & Device Registration            │
            │ ────────────────────────────────────────────────►│
            │     "REGISTER_DEVICE:ESP32S3_MAC"                │ 3. Query Master PostgreSQL DB
            │                                                  │
            │  4. Registration ACK & Custom Greeting           │ 
            │ ◄════════════════════════════════════════════════│ (TTS Greeting streams down based 
            │                                                  │  on room/guest DB profile)
            │  5. User presses PTT Button: "START_RECORDING"   │
            │ ────────────────────────────────────────────────►│ (OLED changes to LISTENING)
            │                                                  │
            │  6. Stream Raw 16kHz PCM Audio Chunks            │
            │ ════════════════════════════════════════════════►│
            │  7. User releases PTT Button: "STOP_RECORDING"   │
            │ ────────────────────────────────────────────────►│
            │                                                  │ 8. OpenAI Whisper (STT)
            │                                                  │ 9. Query Client's Local DB API
            │                                                  │ 10. OpenAI GPT-4o-mini (LLM)
            │                                                  │ 11. OpenAI TTS (Raw 24kHz PCM)
            │                                                  │
            │  12. Stream Audio Chunks back to ESP32           │
            │ ◄════════════════════════════════════════════════│ (OLED changes to SPEAKING)
            │                                                  │
            ▼                                                  ▼

```

---

## 🗄️ Database Setup

Run the following DDL script on your PostgreSQL Master Database instance to create the device registry:

```sql
CREATE TABLE device_registry (
    mac_address VARCHAR(64) PRIMARY KEY,          -- Unique ID: 'ESP32S3_A4CF12345678'
    client_id VARCHAR(100) NOT NULL,              -- Customer ID: 'hotel_taj_mumbai'
    room_number VARCHAR(50) NOT NULL,             -- Room Assignment: 'Suite 401'
    client_db_endpoint VARCHAR(255) NOT NULL,     -- Client API: 'https://api.tajhotels.com/context'
    client_db_auth_token VARCHAR(255),            -- Bearer Auth Token (Optional)
    status VARCHAR(20) DEFAULT 'ACTIVE',          -- Status: 'ACTIVE' or 'SUSPENDED'
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Insert a test device
INSERT INTO device_registry (mac_address, client_id, room_number, client_db_endpoint, status)
VALUES ('ESP32S3_A4CF12345678', 'hotel_taj_mumbai', 'Suite 401', 'https://api.tajhotels.com/context', 'ACTIVE');

```

---

## 🚀 Cloud Deployment Guide

### Step 1: Deploy the Python Backend on a VPS

1. **Clone the code to your Cloud VPS (Ubuntu 22.04 / 24.04).**
2. **Create a `.env` file** in your project directory:

```env
OPENAI_API_KEY="sk-proj-YOUR_OPENAI_KEY"
DATABASE_URL="postgresql://user:password@localhost:5432/your_db"
PORT="8000"

```

3. **Set up a Systemd Service:**
Configure a `systemd` service file so the Python WebSocket server runs 24/7 in the background and auto-restarts on server reboots.
4. **Configure Nginx as a Reverse Proxy with SSL:**
Because the ESP32 must connect securely over the public internet, you cannot expose the Python port directly.

* Install Nginx.
* Map your domain (e.g., `api.my-ai-server.com`) to your VPS IP.
* Use **Certbot (Let's Encrypt)** to generate a free SSL certificate.
* Configure Nginx to route incoming WSS traffic on port `443` to your internal Python server on port `8000`.

---

### Step 2: Flash the ESP32-S3 Firmware

1. Open the C++ sketch in Arduino IDE.
2. Update the `server_url` variable with your production cloud domain:

```cpp
const char* server_url = "api.my-ai-server.com"; // Do not include https:// or wss://
const int   server_port = 443;                   // Standard SSL/TLS port

```

3. Flash the code to your ESP32-S3.

---

### Step 3: Wi-Fi Provisioning (First Boot)

1. Power on the ESP32-S3. The device will play a **Boot Chime**.
2. If no saved Wi-Fi is found, it creates a hotspot named **`ESP32-Audio-Setup`**.
3. Connect your smartphone to **`ESP32-Audio-Setup`** (Password: **`12345678`**).
4. Select your local Wi-Fi, enter the password, and tap **Save**.
5. The device connects, plays a **Happy Wi-Fi Chime**, securely registers its MAC address with your cloud VPS, and automatically plays a **Cloud-Generated Welcome Greeting**.

---

## 🖐️ Usage & System States

* **To Speak:** Press and hold the **Push-to-Talk (PTT)** button. The OLED eyes will widen (LISTENING state). Speak your query and release the button.
* **To Change Wi-Fi:** Press and hold the **Wi-Fi Reset (GPIO 4)** button for 2 seconds. The device will erase its stored networks and reboot into portal mode.
* **Audio Alerts:** The device uses a local mathematical synthesizer to play a happy chime when connecting to the server, and a falling error chime if Wi-Fi or the cloud server drops.

---

## ⚡ Troubleshooting

* **Blank OLED Screen:**
* Ensure you are using **GPIO 8** and **GPIO 9** for SDA/SCL. The default ESP32 pins (21/22) do not work out-of-the-box on the ESP32-S3 without explicitly defining them in `Wire.begin(8, 9)`.


* **Harsh Audio / Static Clipping:**
* Double-check your hardware gain. The MAX98357A `GAIN` pin should be tied to VIN, and the digital multiplier in the C++ code should remain at `1.5`. Pushing digital gain too high causes mathematical clipping.


* **Power Warnings:**
* If using a 4.5V battery pack, wire it *only* to the **VIN / 5V** pin of the ESP32 and the **VIN** of the amplifier. Never feed 4.5V into a 3.3V pin.
