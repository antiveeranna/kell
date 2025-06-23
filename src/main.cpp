#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define CS_PIN 11
#define DEBOUNCE_TIME 100
#define BUFFER_SIZE 3
#define PCF8574_ADDR 0x20
#define SDA_PIN 8
#define SCL_PIN 9
#define INT_PIN 7 // 10k pull-up resistor required!
#define LED_PIN 21
#define NUM_PIXELS 1
#define FLASH_DELAY 400
bool ledState = LOW;

Adafruit_NeoPixel strip(NUM_PIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

TaskHandle_t keypadTaskHandle = NULL;
TaskHandle_t countdownTaskHandle = NULL;
TaskHandle_t countupTaskHandle = NULL;

volatile bool keyPressed = false;
unsigned long lastKeyTime = 0;
char lastKey = 0;
char inputBuffer[BUFFER_SIZE] = { 0 };
int bufferIndex = 0;
int countdownValue = -1; // Holds the countdown start value
int countupValue = -1;   // Holds the count-up end value

const char keypadMap[4][4] = {
    {'D', '#', '0', '*'},
    {'C', '9', '8', '7'},
    {'B', '6', '5', '4'},
    {'A', '3', '2', '1'} };

void IRAM_ATTR handleInterrupt()
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (keypadTaskHandle != NULL)
    {
        vTaskNotifyGiveFromISR(keypadTaskHandle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

void writePCF8574(byte data)
{
    Wire.beginTransmission(PCF8574_ADDR);
    Wire.write(data);
    Wire.endTransmission();
}

byte readPCF8574()
{
    Wire.requestFrom(PCF8574_ADDR, 1);
    return Wire.available() ? Wire.read() : 0xFF;
}

void resetKeypad()
{
    for (int attempt = 0; attempt < 3; attempt++)
    {
        delay(5);
        writePCF8574(0xFF); // Safe state
        delay(2);
        writePCF8574(0xF0); // Columns HIGH
        delay(2);

        byte state = readPCF8574();
        if (state != 0xFF)
            return; // Success
    }

    Serial.println("WARNING: PCF8574 did not respond after reset"); // Crucial delay for INT reset
}

char readKeypadFlipped()
{
    for (int col = 0; col < 4; col++)
    {
        byte colMask = ~(1 << (col + 4)) & 0xF0;
        writePCF8574(colMask | 0x0F); // Activate one column
        delayMicroseconds(500);

        byte rowData = readPCF8574() & 0x0F;

        if (rowData != 0x0F)
        {
            for (int row = 0; row < 4; row++)
            {
                if (!(rowData & (1 << row)))
                {
                    resetKeypad();
                    return keypadMap[row][col];
                }
            }
        }
    }

    resetKeypad();
    return 0;
}

void toggleLED()
{
    ledState = !ledState;

    if (ledState) {
        strip.setPixelColor(0, strip.Color(0, 50, 0)); // Set first pixel to red
    }
    else {
        strip.clear();
    }

    strip.show();
}

void sendToMax7221(uint8_t address, uint8_t value)
{
    digitalWrite(CS_PIN, LOW);
    SPI.transfer(address);
    SPI.transfer(value);
    digitalWrite(CS_PIN, HIGH);
}

void displayNumberOnMax7221(int number)
{
    int tens = number / 10;
    int ones = number % 10;

    uint8_t digit1 = tens > 0 ? tens : 0x0F;
    uint8_t digit2 = ones;

    sendToMax7221(0x01, digit1);
    sendToMax7221(0x02, digit2);
    sendToMax7221(0x03, 0);
    sendToMax7221(0x04, 0);
}

void debugDisplay(int number, uint8_t textSize = 3, int cursorX = 20, int cursorY = 10)
{
    display.clearDisplay();
    display.setTextSize(textSize);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(cursorX, cursorY);
    display.printf("%02d", number);
    display.display();
}

void flashFinalNumber(int number)
{
    for (int i = 0; i < 6; i++)
    {
        // Clear display for flashing effect
        sendToMax7221(0x01, 0x0F);
        sendToMax7221(0x02, 0x0F);
        sendToMax7221(0x03, 0x0F);
        sendToMax7221(0x04, 0x0F);
        display.clearDisplay();
        display.display();
        vTaskDelay(pdMS_TO_TICKS(FLASH_DELAY));

        displayNumberOnMax7221(number);
        debugDisplay(number);
        vTaskDelay(pdMS_TO_TICKS(FLASH_DELAY));
    }
}

void countdownTask(void* pvParameters)
{
    while (1)
    {
        if (countdownValue > 0)
        {
            for (; countdownValue >= 0; countdownValue--)
            {
                Serial.printf("Countdown: %d\n", countdownValue);
                displayNumberOnMax7221(countdownValue);
                debugDisplay(countdownValue);
                toggleLED();
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            flashFinalNumber(0);
            countdownValue = -1; // Stop counting
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Count-up Task - Increments the number every second
void countupTask(void* pvParameters)
{
    while (1)
    {
        if (countupValue > 0)
        {
            for (int i = 0; i <= countupValue; i++)
            {
                Serial.printf("Counting Up: %d\n", i);
                displayNumberOnMax7221(i);
                debugDisplay(i);
                toggleLED();

                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            flashFinalNumber(countupValue);
            countupValue = -1; // Stop counting
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Keypad Task - Only runs when an interrupt occurs
void keypadTask(void* pvParameters)
{
    while (1)
    {
        // Wait until notified by the ISR (handleInterrupt)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(50)); // Debounce delay
        char key = readKeypadFlipped();

        if (key && (key != lastKey || millis() - lastKeyTime > DEBOUNCE_TIME))
        {
            Serial.printf("Key Pressed: %c\n", key);

            if (key == '#')
            { // Clear buffer when '#' is pressed
                bufferIndex = 0;
                inputBuffer[0] = '\0';
            }
            else if (isdigit(key) && bufferIndex < 2)
            { // Store up to 2-digit numbers
                inputBuffer[bufferIndex++] = key;
                inputBuffer[bufferIndex] = '\0';
            }
            else if (key == 'A' && bufferIndex > 0)
            { // Start countdown on 'A'
                countdownValue = atoi(inputBuffer);
                countupValue = -1;

                Serial.printf("Starting countdown from: %d\n", countdownValue);
                bufferIndex = 0;
                inputBuffer[0] = '\0';
            }
            else if (key == 'B' && bufferIndex > 0)
            { // Start count-up on 'B'
                countupValue = atoi(inputBuffer);
                countdownValue = -1;
                Serial.printf("Starting count-up to: %d\n", countupValue);
                bufferIndex = 0;
                inputBuffer[0] = '\0';
            }

            display.clearDisplay();
            display.setTextSize(2);
            display.setTextColor(SSD1306_WHITE);
            display.setCursor(20, 10);
            display.printf("%s", inputBuffer);
            display.display();
            lastKey = key;
            lastKeyTime = millis();
        }
    }
}

void blinkLED()
{
    for (int i = 0; i < 4; i++)
    {
        toggleLED();
        delay(100);
        toggleLED();
        delay(100);
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Starting setup...");

    strip.begin();

    Wire.begin(SDA_PIN, SCL_PIN);

    pinMode(INT_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    pinMode(CS_PIN, OUTPUT);

    toggleLED();
    delay(1000);
    toggleLED();

    attachInterrupt(digitalPinToInterrupt(INT_PIN), handleInterrupt, CHANGE);
    resetKeypad();

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    {
        Serial.println(F("SSD1306 allocation failed"));
        //for (;;)
        //    ; // Stop execution
    }

    blinkLED();

    SPI.begin(12, 19, 13, CS_PIN);
    digitalWrite(CS_PIN, HIGH);

    sendToMax7221(0x0F, 0x00); // Disable Display Test Mode
    sendToMax7221(0x0C, 0x01); // Turn on (Shutdown Mode OFF)
    sendToMax7221(0x0B, 0x03); // Set scan limit (all digits on)
    sendToMax7221(0x09, 0xFF); // Enable BCD decoding (for 7-segment numbers)
    sendToMax7221(0x0A, 0x0f); // Set brightness (0x00 to 0x0F)

    for (int i = 1; i <= 4; i++)
    {
        sendToMax7221(i, 0x0f);
    }

    // Briefly show the digit 8 on all four digits, one by one
    for (int i = 1; i <= 4; i++)
    {
        sendToMax7221(i, 8);
        delay(400);
        sendToMax7221(i, 0x0F);
    }
    // delay(500);
    // for (int i = 1; i <= 4; i++)
    //{
    //  Clear digit
    //}

    // display.stopscroll();
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 10);
    byte initialState = readPCF8574();
    char stateBuffer[32];
    sprintf(stateBuffer, "Initial PCF8574: 0x%02X", initialState);
    display.println(stateBuffer);
    display.display();
    delay(1500);

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 10);
    display.println("MAX7221 Initialized");
    display.display();
    delay(1500);

    // display.stopscroll();
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 10);
    display.println("Waiting for input...");
    display.display();

    xTaskCreatePinnedToCore(keypadTask, "KeypadTask", 4096, NULL, 1, &keypadTaskHandle, 1);
    xTaskCreatePinnedToCore(countdownTask, "CountdownTask", 4096, NULL, 2, &countdownTaskHandle, 1);
    xTaskCreatePinnedToCore(countupTask, "CountupTask", 4096, NULL, 2, &countupTaskHandle, 1);

}

void loop()
{
}