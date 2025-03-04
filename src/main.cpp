#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define CS_PIN 5
#define DEBOUNCE_TIME 100
#define BUFFER_SIZE 32
#define PCF8574_ADDR 0x20
#define SDA_PIN 21
#define SCL_PIN 22
#define INT_PIN 34 // 10k pull-up resistor required!
#define LED_PIN 2

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

TaskHandle_t max7221TaskHandle = NULL;
TaskHandle_t keypadTaskHandle = NULL;

volatile bool keyPressed = false;
unsigned long lastKeyTime = 0;
char lastKey = 0;
char inputBuffer[BUFFER_SIZE] = {0};
int bufferIndex = 0;

const char keypadMap[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}};

/*void IRAM_ATTR handleInterrupt()
{
    keyPressed = true;
}*/

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
    writePCF8574(0x0F); // All rows HIGH
    delay(2);           // Crucial delay for INT reset
}

char readKeypad()
{
    for (int row = 0; row < 4; row++)
    {
        byte rowMask = ~(1 << row) & 0x0F;
        writePCF8574(rowMask | 0xF0); // Activate one row
        delayMicroseconds(500);       // 1000?

        byte colData = readPCF8574() & 0xF0;

        if (colData != 0xF0)
        {
            for (int col = 0; col < 4; col++)
            {
                if (!(colData & (1 << (col + 4))))
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

void sendToMax7221(uint8_t address, uint8_t value)
{
    digitalWrite(CS_PIN, LOW);
    SPI.transfer(address);
    SPI.transfer(value);
    digitalWrite(CS_PIN, HIGH);
}

void max7221Task(void *pvParameters)
{
    while (1)
    {
        for (int i = 0; i < 10; i++)
        {
            Serial.printf("Updating Display: %d\n", i);
            sendToMax7221(0x01, i);
            sendToMax7221(0x02, i + 1);

            digitalWrite(LED_PIN, HIGH);
            vTaskDelay(pdMS_TO_TICKS(200));
            digitalWrite(LED_PIN, LOW);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    }
}

// Keypad Task - Only runs when an interrupt occurs
void keypadTask(void *pvParameters)
{
    while (1)
    {
        // Wait until notified by the ISR (handleInterrupt)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(50)); // Debounce delay
        char key = readKeypad();

        if (key && (key != lastKey || millis() - lastKeyTime > DEBOUNCE_TIME))
        {
            Serial.printf("Key Pressed: %c\n", key);

            if (key == '#')
            { // Clear buffer when '#' is pressed
                bufferIndex = 0;
                inputBuffer[0] = '\0';
            }
            else if (key == '*' && bufferIndex > 0)
            { // Backspace with '*'
                bufferIndex--;
                inputBuffer[bufferIndex] = '\0';
            }
            else if (bufferIndex < BUFFER_SIZE - 1)
            { // Add key to buffer
                inputBuffer[bufferIndex++] = key;
                inputBuffer[bufferIndex] = '\0';
            }

            display.clearDisplay();
            display.setTextSize(2);
            display.setTextColor(SSD1306_WHITE);
            display.setCursor(0, 10);
            display.println(inputBuffer);
            display.display();
            lastKey = key;
            lastKeyTime = millis();
        }
    }
}

void setup()
{
    Serial.begin(115200);

    Wire.begin(SDA_PIN, SCL_PIN);

    pinMode(INT_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(INT_PIN), handleInterrupt, CHANGE);
    resetKeypad();

    pinMode(2, OUTPUT);

    for (int i = 0; i < 4; i++)
    {
        digitalWrite(LED_PIN, HIGH);
        delay(100);
        digitalWrite(LED_PIN, LOW);
        delay(100);
    }

    SPI.begin();
    pinMode(CS_PIN, OUTPUT);
    digitalWrite(CS_PIN, HIGH);

    sendToMax7221(0x0C, 0x01); // Turn on (Shutdown Mode OFF)
    sendToMax7221(0x0F, 0x00); // Disable Display Test Mode
    sendToMax7221(0x09, 0xFF); // Enable BCD decoding (for 7-segment numbers)
    sendToMax7221(0x0A, 0x0a); // Set brightness (0x00 to 0x0F)
    sendToMax7221(0x0B, 0x01); // Set scan limit (all digits on)

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    {
        Serial.println(F("SSD1306 allocation failed"));
        for (;;)
            ; // Stop execution
    }

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 10);
    display.println("MAX7221 Initialized");
    display.display();
    delay(2000);
    display.clearDisplay();
    display.setTextSize(3);

    xTaskCreatePinnedToCore(
        max7221Task,
        "MAX7221Task",
        2048,
        NULL,
        1,
        &max7221TaskHandle,
        1);

    xTaskCreatePinnedToCore(
        keypadTask,
        "KeypadTask",
        4096,
        NULL,
        1,
        &keypadTaskHandle,
        1);
}

void loop()
{
}