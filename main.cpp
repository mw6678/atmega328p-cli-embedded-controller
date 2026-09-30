#include <Arduino.h>
#include <LiquidCrystal.h>
#include <Servo.h>
#include <EEPROM.h>
#include <avr/wdt.h>
#include "RingBuffer.h"

// 1. 하드웨어 핀 정의
constexpr uint8_t PIN_BTN_START = 2;
constexpr uint8_t PIN_BTN_RESET = 3;
constexpr uint8_t PIN_LED_STATUS = 4;
constexpr uint8_t PIN_SERVO      = 5;
constexpr uint8_t PIN_BUZZER     = 6;
constexpr uint8_t PIN_POT        = A0;

// LCD (RS, E, D4, D5, D6, D7)
LiquidCrystal lcd(7, 8, 9, 10, 11, 12);
Servo actServo;

// 2. FSM 상태 정의
enum class SystemState { IDLE, RUNNING, ALARM, CONFIG };
SystemState currentState = SystemState::IDLE;

// 3. 글로벌 제어 변수 및 EEPROM 주소
constexpr uint16_t EEPROM_ADDR_THRESH = 0x00;
uint16_t alarmThreshold = 600; // 기본 경고 기준치 (0~1023)
uint16_t currentAdcVal = 0;

// 4. CLI 링 버퍼 및 커맨드 수신 버퍼
RingBuffer<64> rxBuffer;
char cliLine[32];
uint8_t cliLineIdx = 0;

// 5. Timer1 기반 스케줄러 타이머 카운터 (ms)
volatile uint32_t systemTicks = 0;
uint32_t lastTask10ms = 0;
uint32_t lastTask100ms = 0;
uint32_t lastTask500ms = 0;

// ==========================================
// Timer1 CTC Mode Interrupt (1ms Tick)
// ==========================================
ISR(TIMER1_COMPA_vect) {
    systemTicks++;
}

// UART RX ISR 대체 수신 스캔
void serialEvent() {
    while (Serial.available()) {
        char c = Serial.read();
        rxBuffer.push(c);
    }
}

// ==========================================
// EEPROM 읽기 / 쓰기
// ==========================================
void loadConfiguration() {
    EEPROM.get(EEPROM_ADDR_THRESH, alarmThreshold);
    if (alarmThreshold > 1023) { // 초기 미설정 상태 예외 처리
        alarmThreshold = 600;
        EEPROM.put(EEPROM_ADDR_THRESH, alarmThreshold);
    }
}

void saveThreshold(uint16_t newThresh) {
    alarmThreshold = newThresh;
    EEPROM.put(EEPROM_ADDR_THRESH, alarmThreshold);
}

// ==========================================
// CLI 명령어 파서 (Command Line Interface)
// ==========================================
void parseCLICommand(const char* cmd) {
    Serial.print(F("\r\n[CLI Exec]: "));
    Serial.println(cmd);

    if (strcmp(cmd, "STATUS") == 0) {
        Serial.print(F("State: "));
        switch(currentState) {
            case SystemState::IDLE: Serial.println(F("IDLE")); break;
            case SystemState::RUNNING: Serial.println(F("RUNNING")); break;
            case SystemState::ALARM: Serial.println(F("ALARM")); break;
            case SystemState::CONFIG: Serial.println(F("CONFIG")); break;
        }
        Serial.print(F("ADC Sensor: ")); Serial.println(currentAdcVal);
        Serial.print(F("Threshold: ")); Serial.println(alarmThreshold);
        Serial.print(F("System Uptime: ")); Serial.print(systemTicks / 1000); Serial.println(F("s"));
    } 
    else if (strcmp(cmd, "START") == 0) {
        if (currentState == SystemState::IDLE) {
            currentState = SystemState::RUNNING;
            Serial.println(F("System STARTED."));
        }
    } 
    else if (strcmp(cmd, "STOP") == 0) {
        currentState = SystemState::IDLE;
        Serial.println(F("System STOPPED."));
    } 
    else if (strcmp(cmd, "RESET") == 0) {
        if (currentState == SystemState::ALARM) {
            currentState = SystemState::IDLE;
            noTone(PIN_BUZZER);
            Serial.println(F("Alarm CLEARED. Returned to IDLE."));
        }
    } 
    else if (strncmp(cmd, "SET THRESH ", 11) == 0) {
        int val = atoi(cmd + 11);
        if (val >= 0 && val <= 1023) {
            saveThreshold((uint16_t)val);
            Serial.print(F("Threshold updated to: ")); Serial.println(val);
        } else {
            Serial.println(F("ERROR: Range must be 0~1023"));
        }
    } 
    else if (strcmp(cmd, "HELP") == 0) {
        Serial.println(F("Available Commands:"));
        Serial.println(F("  STATUS          - Show system diagnostics"));
        Serial.println(F("  START / STOP    - Control system operation"));
        Serial.println(F("  RESET           - Clear ALARM state"));
        Serial.println(F("  SET THRESH <val>- Set alarm threshold (0-1023) & save EEPROM"));
    } 
    else {
        Serial.println(F("Unknown Command. Type 'HELP' for commands."));
    }
    Serial.print(F("CLI> "));
}

void processCLIBuffer() {
    char c;
    while (rxBuffer.pop(c)) {
        if (c == '\r' || c == '\n') {
            if (cliLineIdx > 0) {
                cliLine[cliLineIdx] = '\0';
                parseCLICommand(cliLine);
                cliLineIdx = 0;
            }
        } else if (cliLineIdx < sizeof(cliLine) - 1) {
            cliLine[cliLineIdx++] = c;
            Serial.print(c); // Echo
        }
    }
}

// ==========================================
// 태스크 스케줄러 함수들 (Non-blocking)
// ==========================================
void task10ms_ButtonScan() {
    static bool prevBtn1 = HIGH;
    static bool prevBtn2 = HIGH;

    bool currBtn1 = digitalRead(PIN_BTN_START);
    bool currBtn2 = digitalRead(PIN_BTN_RESET);

    // 버튼 1 (START / STOP 토글)
    if (prevBtn1 == HIGH && currBtn1 == LOW) {
        if (currentState == SystemState::IDLE) currentState = SystemState::RUNNING;
        else if (currentState == SystemState::RUNNING) currentState = SystemState::IDLE;
    }

    // 버튼 2 (ALARM 리셋)
    if (prevBtn2 == HIGH && currBtn2 == LOW) {
        if (currentState == SystemState::ALARM) {
            currentState = SystemState::IDLE;
            noTone(PIN_BUZZER);
        }
    }

    prevBtn1 = currBtn1;
    prevBtn2 = currBtn2;
}

void task100ms_LCDUpdate() {
    lcd.setCursor(0, 0);
    switch (currentState) {
        case SystemState::IDLE:    lcd.print("STATE: IDLE    "); break;
        case SystemState::RUNNING: lcd.print("STATE: RUNNING "); break;
        case SystemState::ALARM:   lcd.print("STATE: !!ALARM!"); break;
        case SystemState::CONFIG:  lcd.print("STATE: CONFIG  "); break;
    }

    lcd.setCursor(0, 1);
    lcd.print("ADC:"); lcd.print(currentAdcVal);
    lcd.print(" TH:"); lcd.print(alarmThreshold);
    lcd.print("   ");
}

void task500ms_SensorAndActuator() {
    currentAdcVal = analogRead(PIN_POT);

    // FSM 상태별 동작 수행
    switch (currentState) {
        case SystemState::IDLE:
            digitalWrite(PIN_LED_STATUS, LOW);
            actServo.write(0);
            noTone(PIN_BUZZER);
            break;

        case SystemState::RUNNING:
            digitalWrite(PIN_LED_STATUS, HIGH);
            // 센서 수치에 따라 서보모터 각도 매핑 (0~180도)
            actServo.write(map(currentAdcVal, 0, 1023, 0, 180));

            // 임계치 초과 시 ALARM 상태 전환
            if (currentAdcVal > alarmThreshold) {
                currentState = SystemState::ALARM;
            }
            break;

        case SystemState::ALARM:
            digitalWrite(PIN_LED_STATUS, !digitalRead(PIN_LED_STATUS)); // 토글 점멸
            tone(PIN_BUZZER, 2000); // 2kHz 경고음
            actServo.write(180);
            break;

        case SystemState::CONFIG:
            break;
    }
}

// ==========================================
// Setup & Loop
// ==========================================
void setup() {
    Serial.begin(9600);

    // GPIO 설정
    pinMode(PIN_BTN_START, INPUT_PULLUP);
    pinMode(PIN_BTN_RESET, INPUT_PULLUP);
    pinMode(PIN_LED_STATUS, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(LED_BUILTIN, OUTPUT);

    actServo.attach(PIN_SERVO);
    actServo.write(0);

    lcd.begin(16, 2);
    loadConfiguration();

    // Timer1 설정 (1ms CTC Mode)
    cli(); // 인터럽트 중지
    TCCR1A = 0;
    TCCR1B = 0;
    TCNT1  = 0;
    OCR1A = 249; // (16MHz / (64 * 1000Hz)) - 1 = 249
    TCCR1B |= (1 << WGM12);  // CTC 모드
    TCCR1B |= (1 << CS11) | (1 << CS10); // Prescaler 64
    TIMSK1 |= (1 << OCIE1A); // Timer1 Compare Match A Interrupt 허용
    sei(); // 인터럽트 재개

    // 워치독 타이머 설정 (2초)
    wdt_enable(WDTO_2S);

    Serial.println(F("\r\n===================================="));
    Serial.println(F(" Embedded Serial CLI Controller v1.0"));
    Serial.println(F(" Type 'HELP' to see available commands."));
    Serial.println(F("===================================="));
    Serial.print(F("CLI> "));
}

void loop() {
    wdt_reset(); // 워치독 타이머 리셋 (시스템 정상 구동 알림)

    processCLIBuffer(); // CLI 입력 수신 및 파싱

    // Non-blocking 스케줄러 틱 점검
    uint32_t currentTicks = systemTicks;

    if (currentTicks - lastTask10ms >= 10) {
        lastTask10ms = currentTicks;
        task10ms_ButtonScan();
    }

    if (currentTicks - lastTask100ms >= 100) {
        lastTask100ms = currentTicks;
        task100ms_LCDUpdate();
    }

    if (currentTicks - lastTask500ms >= 500) {
        lastTask500ms = currentTicks;
        task500ms_SensorAndActuator();
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); // Heartbeat
    }
}