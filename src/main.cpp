#include <HardwareSerial.h>
#include <ArduinoJson.h>
#define DEBUG_MODE
// Configure Serial2 pins for GSM module
#define RX2_PIN 16
#define TX2_PIN 17
#define GSM_BAUD 115200

// WebSocket settings
const char *WS_HOST = "65.1.56.164";
const int WS_PORT = 8765;
const char *WS_KEY = "dGhlIHNhbXBsZSBub25jZQ==";

// WebSocket frame opcodes
#define WS_OPCODE_TEXT 0x1
#define WS_OPCODE_PING 0x9
#define WS_OPCODE_PONG 0xA

// Timing settings
const unsigned long MESSAGE_INTERVAL = 10; // Send message every 1 second
const unsigned long PING_INTERVAL = 15000; // Send ping every 15 seconds
const unsigned long COMMAND_TIMEOUT = 500; // AT command timeout
const unsigned long RECONNECT_DELAY = 500; // Delay between reconnection attempts

// Error handling settings
const int MAX_RETRIES = 5;       // Maximum errors before reset
const int MAX_INIT_ATTEMPTS = 3; // Maximum initialization attempts
int errorCount = 0;              // Counter for errors
unsigned long lastDataTime = 0;  // Track last successful data transfer

// Global variables
HardwareSerial GSMSerial(2);
String response = "";
unsigned long lastMessageTime = 0;
unsigned long lastPingTime = 0;

String sendATCommand(const char *command, unsigned long waitTime = COMMAND_TIMEOUT)
{
    response = "";
    GSMSerial.println(command);

// Minimal serial debugging
#ifdef DEBUG_MODE
    Serial.print("Sending: ");
    Serial.println(command);
#endif

    unsigned long startTime = millis();
    while ((millis() - startTime) < waitTime)
    {
        if (GSMSerial.available())
        {
            char c = GSMSerial.read();
            response += c;

            // Faster response check
            if (response.endsWith("OK\r\n") || response.endsWith("ERROR\r\n"))
            {
                break;
            }
        }
    }
    return response;
}

// Function to perform software reset
void softwareReset()
{
    Serial.println("Performing software reset...");
    delay(1000);
    ESP.restart();
}

// Function to check device information
bool checkDeviceInfo()
{
    Serial.println("\n--- Checking Device Information ---");

    // Try AT command multiple times
    for (int i = 0; i < 3; i++)
    {
        String atResponse = sendATCommand("AT");
        if (atResponse.indexOf("OK") != -1)
        {
            sendATCommand("AT+CGMM"); // Model
            sendATCommand("AT+CGMR"); // Firmware
            sendATCommand("AT+CGSN"); // IMEI
            return true;
        }
        delay(1000);
    }

    Serial.println("Error: Module not responding");
    return false;
}

// Function to check SIM status
bool checkSIMStatus()
{
    Serial.println("\n--- Checking SIM Status ---");

    String simResponse = sendATCommand("AT+CPIN?");
    if (simResponse.indexOf("+CPIN: READY") == -1)
    {
        Serial.println("Error: SIM not ready");
        return false;
    }

    sendATCommand("AT+CSQ"); // Signal quality
    String regResponse = sendATCommand("AT+CREG?");
    if (regResponse.indexOf("+CREG: 0,1") == -1 && regResponse.indexOf("+CREG: 0,5") == -1)
    {
        Serial.println("Error: Not registered to network");
        return false;
    }

    return true;
}

// Function to initialize network connection
bool initializeNetwork()
{
    Serial.println("\n--- Initializing Network ---");

    // First, close any existing connections
    sendATCommand("AT+CIPCLOSE=0", 1000);
    sendATCommand("AT+NETCLOSE", 1000);
    delay(1000);

    // Check and activate PDP context
    String cgactStatus = sendATCommand("AT+CGACT?");
    if (cgactStatus.indexOf("+CGACT: 1,1") == -1)
    {
        for (int i = 0; i < 3; i++)
        {
            if (sendATCommand("AT+CGACT=1,1", 10000).indexOf("OK") != -1)
            {
                break;
            }
            if (i == 2)
            {
                Serial.println("Error: Cannot activate PDP context");
                return false;
            }
            delay(1000);
        }
    }

    // Open network
    for (int i = 0; i < 3; i++)
    {
        sendATCommand("AT+NETOPEN", 10000);
        String netStatus = sendATCommand("AT+NETOPEN?");
        if (netStatus.indexOf("+NETOPEN: 1") != -1)
        {
            // Verify IP address
            String ipResponse = sendATCommand("AT+IPADDR");
            if (ipResponse.indexOf("+IPADDR:") != -1)
            {
                return true;
            }
        }
        delay(1000);
    }

    Serial.println("Error: Cannot open network");
    return false;
}

// Function to establish WebSocket connection
bool establishWebSocket()
{
    Serial.println("\n--- Establishing WebSocket Connection ---");

    // Open TCP connection
    String cmd = "AT+CIPOPEN=0,\"TCP\",\"" + String(WS_HOST) + "\"," + String(WS_PORT);
    String tcpResponse = sendATCommand(cmd.c_str(), 10000);
    if (tcpResponse.indexOf("OK") == -1)
    {
        Serial.println("Error: TCP connection failed");
        return false;
    }

    // Wait for connection confirmation
    unsigned long startTime = millis();
    bool connectionConfirmed = false;
    while ((millis() - startTime) < 5000)
    {
        if (GSMSerial.available())
        {
            String resp = GSMSerial.readString();
            Serial.print(resp);
            if (resp.indexOf("+CIPOPEN: 0,0") != -1)
            {
                connectionConfirmed = true;
                break;
            }
        }
        delay(10);
    }

    if (!connectionConfirmed)
    {
        Serial.println("Error: TCP connection timeout");
        return false;
    }

    // Create WebSocket handshake
    String handshake = "GET / HTTP/1.1\r\n";
    handshake += "Host: " + String(WS_HOST) + ":" + String(WS_PORT) + "\r\n";
    handshake += "Upgrade: websocket\r\n";
    handshake += "Connection: Upgrade\r\n";
    handshake += "Sec-WebSocket-Key: " + String(WS_KEY) + "\r\n";
    handshake += "Sec-WebSocket-Version: 13\r\n";
    handshake += "\r\n";

    // Send handshake
    String sendCmd = "AT+CIPSEND=0," + String(handshake.length());
    if (sendATCommand(sendCmd.c_str(), 2000).indexOf(">") != -1)
    {
        GSMSerial.print(handshake);

        // Wait for handshake response
        startTime = millis();
        String handshakeResponse = "";

        while ((millis() - startTime) < 500)
        {
            if (GSMSerial.available())
            {
                char c = GSMSerial.read();
                handshakeResponse += c;
                Serial.write(c);

                if (handshakeResponse.indexOf("101 Switching Protocols") != -1)
                {
                    Serial.println("\nWebSocket connection established!");
                    lastDataTime = millis();
                    return true;
                }
            }
        }
    }

    Serial.println("WebSocket handshake failed");
    return false;
}

// Function to check if connection is valid
bool isConnectionValid()
{
    // First check if module is responsive
    if (sendATCommand("AT").indexOf("OK") == -1)
    {
        return false;
    }

    // Check network registration
    String regStatus = sendATCommand("AT+CREG?");
    if (regStatus.indexOf("+CREG: 0,1") == -1 && regStatus.indexOf("+CREG: 0,5") == -1)
    {
        return false;
    }

    // Check PDP context
    String pdpStatus = sendATCommand("AT+CGACT?");
    if (pdpStatus.indexOf("+CGACT: 1,1") == -1)
    {
        return false;
    }

    // Check network status
    String netStatus = sendATCommand("AT+NETOPEN?");
    if (netStatus.indexOf("+NETOPEN: 1") == -1)
    {
        return false;
    }

    return true;
}

// Function to handle reconnection
bool handleReconnection()
{
    Serial.println("Checking connection status...");

    if (isConnectionValid())
    {
        Serial.println("Base connection valid, re-establishing WebSocket...");
        if (establishWebSocket())
        {
            errorCount = 0;
            return true;
        }
    }

    Serial.println("Connection invalid, performing full reset...");

    // Close existing connections
    sendATCommand("AT+CIPCLOSE=0", 1000);
    sendATCommand("AT+NETCLOSE", 1000);
    delay(RECONNECT_DELAY);

    // Try full reconnection sequence
    for (int i = 0; i < MAX_INIT_ATTEMPTS; i++)
    {
        Serial.printf("Reconnection attempt %d/%d\n", i + 1, MAX_INIT_ATTEMPTS);

        if (initializeNetwork() && establishWebSocket())
        {
            errorCount = 0;
            Serial.println("Reconnection successful");
            return true;
        }
        delay(RECONNECT_DELAY);
    }

    Serial.println("Reconnection failed");
    return false;
}

// Function to handle connection errors
void handleConnectionError()
{
    errorCount++;
    Serial.printf("Connection error #%d of %d\n", errorCount, MAX_RETRIES);

    if (errorCount >= MAX_RETRIES)
    {
        Serial.println("Max retries reached! Performing software reset...");
        softwareReset();
        return;
    }

    if (!handleReconnection())
    {
        errorCount = MAX_RETRIES; // Force reset on next error
    }
}

// Modified WebSocket frame sending function
bool sendWebSocketFrame(const uint8_t *payload, size_t length, uint8_t opcode)
{
    // Increase max payload size
    if (length > 2048)
    {
        Serial.println("Payload chunk too large");
        return false;
    }

    // Dynamically allocate frame to handle larger payloads
    uint8_t *frame = new uint8_t[length + 14]; // Increased buffer size
    uint8_t maskKey[4];

    // Generate mask key
    for (int i = 0; i < 4; i++)
        maskKey[i] = random(256);

    // Create frame header with continuation support
    frame[0] = 0x80 | (opcode & 0x0F);
    frame[1] = 0x80 | (length & 0x7F);
    memcpy(frame + 2, maskKey, 4);

    // Copy and mask payload
    if (length > 0 && payload != nullptr)
    {
        memcpy(frame + 6, payload, length);
        for (size_t i = 0; i < length; i++)
        {
            frame[i + 6] ^= maskKey[i % 4];
        }
    }

    // Optimize send command
    char sendCmd[30];
    snprintf(sendCmd, sizeof(sendCmd), "AT+CIPSEND=0,%d", 6 + length);

    bool success = false;
    if (sendATCommand(sendCmd, 1500).indexOf(">") != -1)
    {
        GSMSerial.write(frame, 6 + length);

        unsigned long startTime = millis();
        while ((millis() - startTime) < 500)
        {
            if (GSMSerial.available())
            {
                String resp = GSMSerial.readString();
                if (resp.indexOf("OK") != -1)
                {
                    lastDataTime = millis();
                    success = true;
                    break;
                }
                if (resp.indexOf("ERROR") != -1)
                    break;
            }
        }
    }

    delete[] frame; // Properly free dynamically allocated memory

    if (!success)
    {
        handleConnectionError();
    }

    return success;
}

// Increase JSON document size to handle more parameters
bool sendMultipleSensorData(const String *names, const float *values, int dataCount)
{
    // Limit data points to 50
    if (dataCount > 50)
    {
        Serial.println("Error: Maximum 50 data points allowed");
        return false;
    }

    // Use a larger JSON document size
    StaticJsonDocument<4096> doc; // Increased to 4KB

    // Populate JSON object with sensor data
    for (int i = 0; i < dataCount; i++)
    {
        doc[names[i]] = values[i];
    }

    // Pre-allocate string with larger buffer
    String jsonString;
    jsonString.reserve(4096); // Match document size
    serializeJson(doc, jsonString);

    // Debug print JSON details
    Serial.print("JSON Length: ");
    Serial.println(jsonString.length());

    // Split large payloads if necessary
    const int MAX_FRAME_SIZE = 1024; // Adjusted based on your GSM module capabilities
    bool success = true;

    // Send data in chunks if too large
    if (jsonString.length() > MAX_FRAME_SIZE)
    {
        // Split into multiple frames
        for (size_t offset = 0; offset < jsonString.length(); offset += MAX_FRAME_SIZE)
        {
            String chunk = jsonString.substring(offset, offset + MAX_FRAME_SIZE);

            success &= sendWebSocketFrame(
                (uint8_t *)chunk.c_str(),
                chunk.length(),
                (offset + chunk.length() >= jsonString.length()) ? WS_OPCODE_TEXT : 0x00);

            if (!success)
                break;

            // Small delay between chunks to prevent overwhelming the module
            delay(50);
        }
    }
    else
    {
        // Send entire payload in one frame
        success = sendWebSocketFrame(
            (uint8_t *)jsonString.c_str(),
            jsonString.length(),
            WS_OPCODE_TEXT);
    }

    return success;
}

// Function to send custom data
bool sendSensorData(const char *name, float value)
{
    StaticJsonDocument<64> doc;
    doc["name"] = name;
    doc["value"] = value;

    String jsonString;
    serializeJson(doc, jsonString);

    return sendWebSocketFrame((uint8_t *)jsonString.c_str(), jsonString.length(), WS_OPCODE_TEXT);
}

void setup()
{
    Serial.begin(115200);
    GSMSerial.begin(GSM_BAUD, SERIAL_8N1, RX2_PIN, TX2_PIN);
    randomSeed(analogRead(0));
    delay(1000); // Wait for GSM module to stabilize

    Serial.println("\nESP32 GSM WebSocket Client Test");

    // Initialize with retry attempts
    bool initialized = false;
    for (int i = 0; i < MAX_INIT_ATTEMPTS; i++)
    {
        Serial.printf("Initialization attempt %d/%d\n", i + 1, MAX_INIT_ATTEMPTS);

        if (checkDeviceInfo() && checkSIMStatus() && initializeNetwork() && establishWebSocket())
        {
            initialized = true;
            break;
        }
        delay(RECONNECT_DELAY);
    }

    if (!initialized)
    {
        Serial.println("Initialization failed after maximum attempts");
        softwareReset();
        return;
    }

    lastMessageTime = millis();
    lastPingTime = millis();
    lastDataTime = millis();
}

void loop()
{
    // Check for connection timeout
    if (millis() - lastDataTime > 30000)
    { // 30 seconds without data
        Serial.println("Connection timeout detected");
        handleConnectionError();
        lastDataTime = millis();
        return;
    }

    // Handle incoming data
    while (GSMSerial.available())
    {
        String data = GSMSerial.readString();
        Serial.print(data);
        lastDataTime = millis();

        if (data.indexOf("+IPCLOSE") != -1 || data.indexOf("ERROR") != -1)
        {
            handleConnectionError();
            return;
        }

        if (data.indexOf("ping") != -1)
        {
            if (!sendWebSocketFrame((uint8_t *)"pong", 4, WS_OPCODE_PONG))
            {
                handleConnectionError();
            }
        }
    }
    if (millis() - lastMessageTime >= MESSAGE_INTERVAL)
    {
        // Prepare sensor names and values
        String sensorNames[16] = {"X0", "X1", "X2", "X3", "X4", "X5", "X6", "X7", "X8", "X9", "X10", "X11", "X12", "X13", "X14", "X15"};
        float sensorValues[16] = {1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 1.10, 1.11, 1.12, 1.13, 1.14, 1.15};
        String sensorNames2[16] = {"X0", "X1", "X2", "X3", "X4", "X5", "X6", "X7", "X8", "X9", "X10", "X11", "X12", "X13", "X14", "X15"};
        float sensorValues2[16] = {1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 1.10, 1.11, 1.12, 1.13, 1.14, 1.15};
        Serial.println("Attempting to send multiple sensor data Batch 1...");
        if (sendMultipleSensorData(sensorNames, sensorValues, 12))
        {
            Serial.println("Multiple sensor data sent successfully");
        }
        else
        {
            Serial.println("Failed to send multiple sensor data");
        }

        Serial.println("Attempting to send multiple sensor data Batch 2...");
        if (sendMultipleSensorData(sensorNames2, sensorValues2, 12))
        {
            Serial.println("Multiple sensor data sent successfully");
        }
        else
        {
            Serial.println("Failed to send multiple sensor data");
        }

        lastMessageTime = millis();
    }

    if (millis() - lastPingTime >= PING_INTERVAL)
    {
        sendWebSocketFrame((uint8_t *)"ping", 4, WS_OPCODE_PING);
        lastPingTime = millis();
    }
}
