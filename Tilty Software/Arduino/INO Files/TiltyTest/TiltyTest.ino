#include <i2c_t3.h>
#include <HMC5883.h>
#include <MPL3115A2.h>

#include <I2Cdev.h>
//#include <MPU6050.h>
#include "MPU6050_6Axis_MotionApps20.h"

//#include <Wire.h> // Uncomment to use standard Wire library on normal Arduinos
#include <i2c_t3.h> // Uncomment to use I2C_t3 Wire library on Teensy 3.0
#include <SPI.h>

#include <SpiFlash.h>

int raw_values[9];

// Places to store the compass reading
int compass_x, compass_y, compass_z;

// Places to store MPU6050 IMU readings
float ypr[3];		   // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector
int16_t ax, ay, az;
int16_t gx, gy, gz;

// Places to store altimeter readings
float altitude, temperature;

// Instantiate a compass 
HMC5883 compass;

// Instantiate the IMU
MPU6050 imu;

// Instantiate the altimeter
MPL3115A2 altimeter;

// Instantiate the flash memory
SpiFlash flash;

bool compass_avail, imu_avail, alt_avail, flash_avail, bt_avail; // variabless to indicate whether sensor is available

// Constant variable #defines
#define VOLTAGE_SENSE_PIN 14
#define VOLTAGE_DIVIDER 0.05061465
#define SS_PIN 6
#define FLASH_TEST_WRITES 2048
#define BT_COMMAND 15
#define BT_DEFAULT_BAUD 38400
#define TILTY_DEFAULT_BT_BAUD 115200

bool display_raw_IMU, send_box_demo;

void setup()
{
	//Open up some serial communications with the computer
	Serial.begin(115200);
	pinMode(BT_COMMAND, OUTPUT);
	digitalWrite(BT_COMMAND, HIGH);
	
	flash.begin(SS_PIN, 2);
	
	while (!Serial) {}
	
	//Start the internal I2C Bus for the sensors 
	Wire.begin(I2C_MASTER, 0, I2C_PINS_18_19, I2C_PULLUP_EXT, I2C_RATE_800);
	
	delay(10);
	
	// initialize the IMU
	imu = MPU6050();
        imu.reset();
        delay(5);
	imu_avail = imu.init();
	Serial.print("IMU status...\t\t\t");
	Serial.println(imu_avail ? "OK!" : "NOT OK!");
	if (imu_avail) {
		setupDMP();
	}
	
	// initialize the compass
	compass_avail = compass.init();
	Serial.print("Compass status...\t\t");
	Serial.println(compass_avail ? "OK!" : "NOT OK!");
	
	// initialize the altimeter and set oversampling to 0 to speed up measurements
	alt_avail = altimeter.init();
	if (alt_avail) {	altimeter.setOversampling(0);}
	Serial.print("Altimeter status...\t\t");
	Serial.println(alt_avail ? "OK!" : "NOT OK!");

        // Check voltage sensing
        float voltage = analogRead(VOLTAGE_SENSE_PIN) * VOLTAGE_DIVIDER;
        Serial.print("Voltage sense status...\t\t");
        Serial.println(voltage < 5.3 && voltage > 4.3 ? "OK!" : "NOT OK!");

	// initialize and check for the flash memory chip
	flash_avail = flash.getManufacturer() == 1 ? true : false;
	Serial.print("Flash memory status...\t\t");
	Serial.println(flash_avail ? "OK!" : "NOT OK!");
        Serial.print("\tMemory Size: ");
        Serial.print(flash.getCapacity());
        Serial.println(" bytes");
	Serial.print("\tMemory read: ");
	Serial.print(checkMemory(FLASH_TEST_WRITES));
	Serial.print(" of ");
	Serial.print(FLASH_TEST_WRITES);
	Serial.println(" reads failed");
	
	// initialize and check for the bluetooth chip
	int baud = findBaud();
	Serial1.begin(baud);
	bt_avail = checkBTok();
	Serial.print("Bluetooth status...\t\t");
	Serial.println(bt_avail ? "OK!" : "NOT OK!\n");
	if (bt_avail) {
		Serial.print("\tBluetooth version: ");
		getBTversion();
		Serial.print("\tBluetooth name set to TiltyBT: ");
		setBTname();
		setBTbaud();
		Serial.print("\tBluetooth baud set to: ");
		Serial.println(findBaud());
		
		Serial.println();
	}
	
	if (imu_avail && !compass_avail && !alt_avail) {	Serial.print("Tilty Duo ");}
	else if (imu_avail && compass_avail && alt_avail) {	Serial.print("Tilty Quad ");}
	
	if (bt_avail) {	Serial.print("w/ bluetooth ");}
	Serial.println("found!");
	
	Serial.println("Enter any character to test sensor reading...");
	
	while (!Serial.available()) {
		if (imu_avail) { while (!imu.getIntDataReadyStatus());}
		if (compass_avail) { while (!compass.getDataReady());}
		if (alt_avail) { while (!altimeter.getDataReady());}
	}
	while (Serial.available()) { Serial.read();}
	
	imu.resetFIFO();
}

void loop()
{
	if (imu_avail) {
		readDMP();
		Serial.print("yaw: ");
		Serial.print(ypr[0]);
		Serial.print("  Pitch: ");
		Serial.print(ypr[1]);
		Serial.print("  Roll: ");
		Serial.print(ypr[2]);
		
		if (display_raw_IMU) {
			imu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
			Serial.print("\t\t Accel x: "); Serial.print(ax);
			Serial.print("  y: "); Serial.print(ay);
			Serial.print("  z: "); Serial.print(az);
		
			Serial.print("\t\tGyro x: "); Serial.print(gx);
			Serial.print("  y: "); Serial.print(gx);
			Serial.print("  z: "); Serial.print(gx);
		}
		
		if (send_box_demo) {
			char YAW = 'Y';
			char PITCH = 'P';
			char ROLL = 'R';
			char BATT = 'B';
			char ALT = 'A';
			char TEMP = 'T';
			Serial.println();
			Serial.print(YAW); Serial.println(ypr[0]);
			Serial.print(PITCH); Serial.println(ypr[1]);
			Serial.print(ROLL); Serial.println(ypr[2]);
		}
	}
	
	if (compass_avail) {
		compass.getValues(&compass_x, &compass_y, &compass_z);
		Serial.print("\t\t Compass x: "); Serial.print(compass_x);
		Serial.print(" y: "); Serial.print(compass_y);
		Serial.print(" z: "); Serial.print(compass_z);
	}
	if (alt_avail) {
		altitude = altimeter.readAltitudeM();
		temperature = altimeter.readTempC();
		altimeter.forceMeasurement();
		Serial.print("\t\t Altitude: "); Serial.print(altitude);
		Serial.print("\t\t Temperature: "); Serial.print(temperature);
	}
	
	Serial.print("\t\t Voltage: "); Serial.print(analogRead(VOLTAGE_SENSE_PIN) * VOLTAGE_DIVIDER);
	
	Serial.println();
	
	if (Serial.available()) {
		char data = Serial.read();
		if (data == 'R') {	display_raw_IMU = !display_raw_IMU;}
		if (data == 'B') {	send_box_demo = !send_box_demo;}
	}
	
	// wait for all three sensors to have new data available
	if (imu_avail) { while (!imu.getIntDataReadyStatus());}
	if (compass_avail) { while (!compass.getDataReady());}
	if (alt_avail) { while (!altimeter.getDataReady());}
}
