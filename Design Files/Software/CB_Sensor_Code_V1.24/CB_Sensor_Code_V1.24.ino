// === Configuration Constants ===
const int micPin = A0;              // Microphone input pin
const int sampleInterval = 100;     // Sampling every 100ms
const int timeBetween = 2000;       // Time between sound checks and possible moves
const int numSamples = timeBetween / sampleInterval;
unsigned long lastHomeTime = 0;
const unsigned long homeInterval = 60000; // 1 minute for now


// === Hardcoded Positions ===
const float Y_DOWN = -28.0;
const float Y_UP   = -19.0;

const float X_1 = -2.0;
const float X_2 = -10.0;
const float X_3 = -19.0;
const float X_4 = -28.0;

const float xPositions[] = { X_1, X_2, X_3, X_4 };
const int numXPositions = sizeof(xPositions) / sizeof(xPositions[0]);
int currentXIndex = 0;

// Track absolute positions from home
float currentXPosCM = xPositions[currentXIndex];
float currentYPosCM = Y_UP;

// Microphone buffer and tracking (volatile for use in ISR)
#define MAX_SAMPLES 100
volatile int micBuffer[MAX_SAMPLES];
volatile int bufferIndex = 0;
volatile long sum = 0;

float currentAverage = 0.0;
float prevAverage = 0.0;

unsigned long lastCheckTime = 0;

void setup() {
    Serial.begin(115200);
    Serial1.begin(115200);
    delay(1000);

    Serial.println("=== Arduino Mega 2560 G-code Sender ===");
    Serial.println("Waiting for Grbl to initialize...");
    delay(2000);

    initializeGRBL();
    homeX();
    homeY();

    Serial.println("Verifying Grbl settings...");
    Serial1.println("$$");

    for (int i = 0; i < MAX_SAMPLES; i++) {
        micBuffer[i] = 0;
    }
    sum = 0;

    setupTimer1();
    Serial.println("Version: v1.24")
}

void loop() {
    unsigned long currentTime = millis();

    // Move Y up
    moveY(Y_UP);
    delay(timeBetween);

    if (currentTime - lastCheckTime >= timeBetween) {
        lastCheckTime = currentTime;

        long safeSum;
        noInterrupts();
        safeSum = sum;
        interrupts();

        prevAverage = currentAverage;
        currentAverage = safeSum / (float)numSamples;

        Serial.print("Current Average: "); Serial.println(currentAverage);
        Serial.print("Previous Average: "); Serial.println(prevAverage);

        float delta = currentAverage - prevAverage;

        if (delta > 0.5 && currentXIndex < numXPositions - 1) {
            Serial.println("louder: stepping left");
            currentXIndex++;
        } else if (delta < -0.5 && currentXIndex > 0) {
            Serial.println("quieter: stepping right");
            currentXIndex--;
        } else {
            Serial.println("Stable sound level: staying in place");
        }

        moveX(xPositions[currentXIndex]);
        getGrblPosition();
        delay(timeBetween);

        // Move Y back down
        moveY(Y_DOWN);
        delay(timeBetween);
    }

    // Manual passthrough
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        if (input.length() > 0) {
            Serial1.println(input);
            Serial.print("↪ Sent to Grbl: ");
            Serial.println(input);
        }
    }

    while (Serial1.available()) {
        String response = Serial1.readStringUntil('\n');
        Serial.print("⤷ Grbl: ");
        Serial.println(response);
    }
  
        // Auto-homing check
    if (millis() - lastHomeTime >= homeInterval) {
        Serial.println("🔁 Rehoming...");
        rehome();
        lastHomeTime = millis();
    }

}

// =======================
// Timer1 Setup
// =======================
void setupTimer1() {
    cli();
    TCCR1A = 0;
    TCCR1B = 0;
    OCR1A = 6249;  // 100ms interval
    TCCR1B |= (1 << WGM12);
    TCCR1B |= (1 << CS12); // 256 prescaler
    TIMSK1 |= (1 << OCIE1A);
    sei();
}

ISR(TIMER1_COMPA_vect) {
    int micValue = analogRead(micPin);
    int absValue = abs(micValue - 512);
    if (absValue > 512) absValue = 512;

    sum -= micBuffer[bufferIndex];
    micBuffer[bufferIndex] = absValue;
    sum += absValue;
    bufferIndex = (bufferIndex + 1) % numSamples;
}

// =======================
// Axis Movement Functions (absolute)
// =======================
void moveX(float targetX) {
    String gcode = "G0 X" + String(targetX, 1);  // 1 decimal place, properly formatted
    sendGcode(gcode);
    currentXPosCM = targetX;
}

void moveY(float targetY) {
    String gcode = "G0 Y" + String(targetY, 1);
    sendGcode(gcode);
    currentYPosCM = targetY;
}


// =======================
// G-code Utilities
// =======================
bool sendGcode(String command) {
    Serial1.println(command);
    Serial.print("Sent to Grbl: ");
    Serial.println(command);

    unsigned long startTime = millis();
    while (true) {
        if (Serial1.available()) {
            String response = Serial1.readStringUntil('\n');
            Serial.print("Grbl Response: ");
            Serial.println(response);
            if (response.indexOf("ok") >= 0) return true;
            if (response.startsWith("error") || response.startsWith("ALARM")) return false;
        }

        if (millis() - startTime > 2000) {
            Serial.println("⛔ ERROR: No response from Grbl (timeout).");
            return false;
        }
    }
}

void getGrblPosition() {
    getGrblAxis('B'); // 'B' = just update both X and Y internally
}

float getGrblAxis(char axis) {
    Serial1.println("?");
    unsigned long startTime = millis();
    String response = "";
    int retries = 10;

    while (retries-- > 0 && millis() - startTime < 1000) {
        if (Serial1.available()) {
            response = Serial1.readStringUntil('\n');
            if (response.startsWith("<")) {
                int mposIndex = response.indexOf("MPos:");
                if (mposIndex >= 0) {
                    int end = response.indexOf("|", mposIndex);
                    String coordPart = response.substring(mposIndex + 5, end);
                    int comma1 = coordPart.indexOf(',');
                    int comma2 = coordPart.indexOf(',', comma1 + 1);

                    float grblX = coordPart.substring(0, comma1).toFloat();
                    float grblY = coordPart.substring(comma1 + 1, comma2).toFloat();

                    currentXPosCM = grblX;
                    currentYPosCM = grblY;

                    if (axis == 'X') return grblX;
                    if (axis == 'Y') return grblY;
                    return 0.0;
                }
            }
        }
    }

    Serial.println("⚠️ Failed to get position from Grbl.");
    return 0.0;
}

// =======================
// Grbl Setup Functions
// =======================
void initializeGRBL() {
    Serial1.println("$X");       // Unlock GRBL
    delay(500);

    Serial1.println("$10=1");    // Show MPos in status (default)
    delay(500);

    Serial1.println("$3=0");     // DEFAULT axis direction — ✅ not inverted
    delay(500);

    Serial1.println("$23=0");    // DEFAULT homing direction — ✅ not inverted
    delay(500);

    Serial1.println("G90");      // Absolute mode
    delay(500);

    Serial1.println("G21");      // mm units
    delay(500);

    Serial1.println("$100=100.000");  // Steps/mm X
    Serial1.println("$110=500.000");  // Max rate X
    Serial1.println("$120=100.000");  // Acceleration X
    Serial1.println("$20=0");         // Soft limits off
    Serial1.println("$5=0");          // Disable invert limit pins
    Serial1.println("$21=0");         // Disable hard limits
    Serial1.println("$22=1");         // Enable homing
    delay(1000);
}

void homeX() {
    Serial1.println("$HX");
    delay(5000);
    Serial1.println("G10 L20 P1 X0");
    delay(500);
    currentXPosCM = 0;
}

void homeY() {
    Serial1.println("$HY");
    delay(5000);
    Serial1.println("G10 L20 P1 Y0");
    delay(500);
    currentYPosCM = 0;
}

void rehome() {
    homeX();
    homeY();
    Serial.println("✅ Rehoming complete.");
}
