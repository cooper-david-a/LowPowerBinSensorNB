#define DEBUG 0

#include <ArduinoLowPower.h>
#include <Arduino_PMIC.h>
#include <HX711.h>
#include <MKRNB.h>
#include "secrets.h"


// Timing
#define ONE_HOUR 3600000
String dateHeader = "";
int localHour = 0;

// Battery
#ifndef ADC_BATTERY
#endif
#define R1 330000
#define R2 1000000
#define BATTERY_FULL_VOLTAGE 4.2
#define BATTERY_EMPTY_VOLTAGE 3.6
#define BATTERY_CAPACITY 6.0
int maxSourceVoltage = 3.3 * (R1 + R2) / R2;
float batteryVoltage;
bool onExternalPower;
bool chargeDone;
bool chargeNeeded;

// Weight
#define DATA_PIN 7
#define CLOCK_PIN 6
HX711 scale;
float weight = -1000;

// Distance
float distance = 0.0;

NB nbAccess(DEBUG);
NBClient client;
GPRS gprs;
bool nbConnected = false;
bool gprsConnected = false;

// Server details
#define PORT 80
#define HOST "insecure.compostonly.com:80"
char server[] = "insecure.compostonly.com";

void setup()
{
#if DEBUG
  Serial.begin(9600);
  while (!Serial)
  {
    ;
  }
#endif

  // Battery management setup
  analogReference(AR_DEFAULT);
  analogReadResolution(12);
  PMIC.begin();
  PMIC.enableBATFET();
  PMIC.setMinimumSystemVoltage(BATTERY_EMPTY_VOLTAGE);
  PMIC.setChargeVoltage(BATTERY_FULL_VOLTAGE);
  PMIC.setChargeCurrent(0.2 * BATTERY_CAPACITY);
  PMIC.setFastChargeTimerSetting(12);


  // Weight measurement setup
  scale.begin(DATA_PIN, CLOCK_PIN);
  scale.set_scale(-3370);
  scale.set_offset(-577221);
}

void loop()
{
  getBatteryStatus();
#if DEBUG
  Serial.print("Battery Voltage: ");
  Serial.println(batteryVoltage);
#endif

  getWeight();
#if DEBUG
  Serial.print("Weight: ");
  Serial.println(weight);
#endif

  getDistance();
#if DEBUG
  Serial.print("Distance: ");
  Serial.println(distance);
#endif

  onExternalPower = PMIC.isPowerGood();

  if(onExternalPower){
    int startChargingTime = millis();
    chargeNeeded = batteryVoltage < 3.7 && batteryVoltage > 3;
#if DEBUG
    Serial.print("Charge Needed: ");
    Serial.println(chargeNeeded);
#endif
    if (chargeNeeded)
    {
    PMIC.enableCharge();
    
    while (millis() - startChargingTime < 12*ONE_HOUR)
    {
      onExternalPower = PMIC.isPowerGood();
      chargeDone = PMIC.chargeStatus() == CHARGE_TERMINATION_DONE;      
      if(!onExternalPower || chargeDone){
        break;        
      }
      delay(10000);
      
#if DEBUG
      getBatteryStatus();
      Serial.print("Battery Voltage: ");
      Serial.println(batteryVoltage);
#endif

    }
      PMIC.disableCharge();
    }
      
  }
  
  sendReport();

  // Shutdown NB connection
  client.stop();
  nbAccess.shutdown();

#if DEBUG
  Serial.println("report sent?");
  Serial.print("Local Hour: ");
  Serial.println(localHour);
  Serial.print("Local Time: ");
  Serial.println(nbAccess.getTime());
  Serial.print("external power:");
  Serial.println(onExternalPower);
#endif

  if (batteryVoltage < BATTERY_EMPTY_VOLTAGE)
  {
    PMIC.disableBATFET();
  }

  if (onExternalPower){
    delay(60000);
  }else
  {
    if (localHour > 15)
    {
      LowPower.deepSleep(12*ONE_HOUR);
    }
    else
    {
      LowPower.deepSleep(ONE_HOUR/2);
    }
  }
}
void getBatteryStatus()
{
  bool done = false;
  batteryVoltage = 0;
  int tries = 0;
  int maxTries = 10;
  do
  {
    float rawADC = analogRead(ADC_BATTERY);
    float voltADC = rawADC * (3.3 / 4095.0);
    batteryVoltage = voltADC * (maxSourceVoltage / 3.3);
    tries++;
    done = batteryVoltage > 0 || tries >= maxTries;
    if (!done)
    {
      delay(1000);
    };
  } while (!done);
}

void getWeight()
{
  bool done = false;
  weight = -1000.0;
  int tries = 0;
  int maxTries = 3;
  scale.power_up();
  do
  {
    if (scale.is_ready())
    {
      weight = scale.get_units(10);
    }
    tries++;
    done = weight > -1000 || tries >= maxTries;
    if (!done)
    {
      delay(1000);
    };
  } while (!done);
  scale.power_down();
}

void getDistance()
{
  Serial1.begin(9600);
  delay(5000);
  bool done = false;
  unsigned char distanceData[4] = {};
  distance = 0;
  int tries = 0;
  int maxTries = 10;

  do
  {
    // Clear Buffer
    while (Serial1.available())
    {
      Serial1.read();
    }
    delay(200);

    do
    {
      for (int i = 0; i < 4; i++)
      {
        distanceData[i] = Serial1.read();
      }
    } while (distanceData[0] != 0xff);

    int sum = (distanceData[0] + distanceData[1] + distanceData[2]) & 0x00FF;

    if (sum == distanceData[3])
    {
      distance = (distanceData[1] << 8) + distanceData[2];
    }

    tries++;
    done = distance > 0 || tries >= maxTries;
    if (!done)
    {
      delay(1000);
    }
  } while (!done);
  Serial1.end();
}

void sendReport()

{
  int connectionTries = 0;
  int maxConnectionTries = 3;
  bool connectionDone = false;
  char body[250];
  localHour = -1;
  dateHeader = "";

#if DEBUG
  Serial.println("Connecting to NB network");
#endif

  do
  {      
#if DEBUG
    Serial.print("connectionTries: ");
    Serial.println(connectionTries);
#endif
    nbConnected = nbAccess.begin(SECRET_PINNUMBER, SECRET_APN) == NB_READY;
    connectionTries++;
    connectionDone = connectionTries >= maxConnectionTries || nbConnected;    
    if (!connectionDone)
    {
      delay(10000);
    }
  } while (!connectionDone);

#if DEBUG
  Serial.print("NB Connected: ");
  Serial.println(nbConnected);
#endif

  if (nbConnected)
  {
    // Connect to GPRS
    connectionTries = 0;
    connectionDone = false;
    do
    {
      gprsConnected = gprs.attachGPRS() == GPRS_READY;
      connectionTries++;
      connectionDone = connectionTries >= maxConnectionTries || gprsConnected;
      #if DEBUG
        Serial.print("connectionTries: ");
        Serial.println(connectionTries);
      #endif
      if (!connectionDone)
      {
        delay(5000);
      }
    } while (!connectionDone);

#if DEBUG
    Serial.print("GPRS Connected: ");
    Serial.println(gprsConnected);
#endif
  }

  connectionTries = 0;
#if DEBUG
  Serial.println("Client Connecting");
#endif

  while (!client.connect(server, PORT))
  {
    connectionTries++;
    if (connectionTries >= maxConnectionTries)
    {
      break;
    }
    delay(5000);
  }

#if DEBUG
  Serial.print("client Connected: ");
  Serial.println(client.connected());
#endif

  if (client.connected())
  {
    int n;
    n = sprintf(body, "{\"ssid\": \"%s\", \"batteryVoltage\": %.2f, \"weight\": %.0f, \"distance\": %.0f, \"onExternalPower\": %d}", SECRET_APN, batteryVoltage, weight, distance, onExternalPower);
    #if DEBUG
      Serial.print("Body: ");
      Serial.println(body);
    #endif
    client.println("POST / HTTP/1.1");
    client.print("Host: ");
    client.println(HOST);
    client.println("Content-Type: application/json");
    client.println("Content-Length: " + String(n));
    //client.println("Connection: close");
    client.println();
    client.println(body);
    delay(1000);

    while (client.available())
    {
      String line = client.readStringUntil('\n');
      
#if DEBUG
      Serial.println(line);
#endif

      if (line.startsWith("Date: "))
      {
        dateHeader = line.substring(6);
        
#if !DEBUG
        break;      
#endif

      }
      if (line == "\r")
      {
        break;
      }
    }
    if (dateHeader != "")
    {
      int hourIndex = dateHeader.indexOf(':') - 2;
      String hourStr = dateHeader.substring(hourIndex, hourIndex + 2);
      localHour = (hourStr.toInt() - 5);
      if (localHour <= 0)
      {
        localHour = localHour + 24;
      }
    }
  }
} 
