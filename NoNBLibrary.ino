/*
  This sketch works with the MKR NB 1500 without the need for the MKRNB library.


  It updates thingspeak fields using http POST every 10 mins.


  Field1 = Battery Voltage
  Field2 = Example data(4)
  Field3 = Modem Reset Count
  Field4 = Modem signal, rsrq in dB


  It also reads command from Field5 using http GET. In the sample program I have provided,
  a command of "ledon" will turn on the onboard LED and "ledoff" will turn off the onboard LED.
  You can use another arduino or a desktop/android app like Postman to send commands
  to Field5.
  With this program, these commands are only updated when the thingspeak channel is updated.
  By default, it updates every 10 mins. Therefore, it only executes commands every 10 mins.
  This can be easily changed by changing the thingspeakPostInterval.


*/


#include <avr/dtostrf.h>
#include "ArduinoLowPower.h"


const char apn[] = "hologram";  //"m2m64.com.attz";
const char simPIN[] = "1234";   //If the sim card needs a PIN, this will be used.
/*
  Network profile ID, see AT Manual Appendix C.4:


  C.4  SARA-R410M-02B Americas MNO profile tables
  Network     Profile ID
  --------     ----------
  Undefined     0
  AT&T          2
  Verizon       3(won't register unless device is approved by Verizon)
  T-mobile US   5
  Sprint        8
  Telus         21
*/
const char networkProfile[] = "2";  //Network profile ID


const char thingspeakKey[] = "KEYYYYYYY";      //Thingspeak api write key
const char thingspeakReadKey[] = "KEYYYYYY";  //Thingspeak api read key
const char thingspeakChannelId[] = "1234567";
int thingspeakCommandField = 5;  //Thingspeak field used to read commands
bool mcuLowPower = false;        //If true, the Microcontroller will be put into sleep after each post.


const char thingspeakUrl[] = "api.thingspeak.com";
const unsigned int thingspeakPort = 80;  //80 or 443 for TLS
const unsigned long thingspeakPostInterval = 600000;


//System variables below ------------------------------------


unsigned long lastThingspeakPostTime = 0;
unsigned long commandStartTime = 0;
unsigned long delayStartTime = 0;
unsigned long postStartTime = 0;


int resetCount = 0;
int responsePosition = 0;
int sendCommandStage = 0;
int stage = 0;
int timeoutCount = 0;
int socket = -1;
int socketErrorCount = 0;
int httpErrorCount = 0;
int loopCount = 0;
int rsrq_dB = 0;
int dnsCheckPeriod = 0;
int dnsCheckCount = 0;
int consumeResponseChars = 0;


bool modemSetupFlag = false;
bool thingspeakSuccessFlag = false;
bool modemHardResetAndPowerOnPending = true;
bool modemReset = false;
bool modemSoftResetPending = false;
bool responseError = false;
bool thingspeakCommand = false;
bool continueResponse = false;
bool smsPending = false;


char response[2000];
char formattedCommand[500];
char formattedHttp[500];
char thingspeakIp[16];
char socketCloseIndicator[11];


void setup() {


  pinMode(SARA_PWR_ON, INPUT);
  pinMode(SARA_RESETN, INPUT);


  Serial.begin(115200);
  SerialSARA.begin(115200);


  dnsCheckPeriod = round(3600000 / thingspeakPostInterval);
  if (dnsCheckPeriod < 1) {
    dnsCheckPeriod = 1;
  }
  dnsCheckCount = dnsCheckPeriod + 1;
}


void loop() {
  if (timeoutCount > 20) {
    timeoutCount = 0;
    resetCount++;
    modemReset = true;
    modemHardResetAndPowerOnPending = true;
  }
  if (modemHardResetAndPowerOnPending) {
    modemHardResetAndPowerOn();
  }
  if (responsePosition > 1999) {
    Serial.print(response);
    memset(response, 0, sizeof(response));
    responsePosition = 0;
  }
  if (SerialSARA.available()) {
    response[responsePosition] = SerialSARA.read();
    if (consumeResponseChars > 0) {
      consumeResponseChars--;
    } else {
      responsePosition++;
    }
  }
  if (!modemSetupFlag && !modemHardResetAndPowerOnPending) {
    modemSetup();
  }
  if (!mcuLowPower && millis() - lastThingspeakPostTime > thingspeakPostInterval) {
    thingspeakSuccessFlag = false;
    lastThingspeakPostTime = millis();
  }
  if (!thingspeakSuccessFlag && modemSetupFlag && !modemHardResetAndPowerOnPending) {
    rawHttpPost();
  }
}


void sendCommand(const char command[500], unsigned long timeout, bool acceptError) {


  if (sendCommandStage == 0) {
    if (!continueResponse) {
      memset(response, 0, sizeof(response));
      responsePosition = 0;
    } else {
      continueResponse = false;
    }
    Serial.println(command);
    SerialSARA.println(command);
    responseError = false;
    commandStartTime = millis();
    sendCommandStage++;
    loopCount = 0;
  }
  if (sendCommandStage == 1) {
    loopCount++;
    if (loopCount > 1000) {
      loopCount = 0;
      if (strstr(response, "\nOK\r") != NULL || strstr(response, "\r\n@") != NULL) {
        delayStartTime = millis();
        sendCommandStage++;
      }
      if (strstr(response, "\nERROR\r") != NULL || strstr(response, "+CME ERROR") != NULL) {
        responseError = true;
        delayStartTime = millis();
        sendCommandStage++;
      }
    }
    if (millis() - commandStartTime > timeout) {
      Serial.println(response);
      Serial.println("Response timeout");
      sendCommandStage = 0;
      stage = 0;
      timeoutCount++;
    }
  }
  if (sendCommandStage == 2) {
    if (millis() - delayStartTime > 25) {
      Serial.println(response);
      timeoutCount = 0;
      sendCommandStage = 0;
      if (responseError && !acceptError) {
        stage = 0;
      } else {
        stage++;
      }
    }
  }
}


void modemHardResetAndPowerOn() {
  if (stage == 0) {
    modemSetupFlag = false;
    if (modemReset) {
      Serial.println("Modem resetting...");
      pinMode(SARA_RESETN, OUTPUT);
      digitalWrite(SARA_RESETN, HIGH);  //Logic high on Arduino pin = logic low on SARA R4
      delayStartTime = millis();
      stage++;
    } else {
      stage = 2;
    }
  }
  if (stage == 1) {
    if (millis() - delayStartTime > 12000) {
      pinMode(SARA_RESETN, INPUT);
      delayStartTime = millis();
      stage++;
    }
  }
  if (stage == 2) {
    if (millis() - delayStartTime > 500) {
      pinMode(SARA_PWR_ON, OUTPUT);
      digitalWrite(SARA_PWR_ON, HIGH);  //Logic high on Arduino pin = logic low on SARA R4
      delayStartTime = millis();
      stage++;
    }
  }
  if (stage == 3) {
    if (millis() - delayStartTime > 500) {
      pinMode(SARA_PWR_ON, INPUT);
      Serial.println("Modem on");
      delayStartTime = millis();
      stage++;
    }
  }
  if (stage == 4) {
    if (millis() - delayStartTime > 5000) {
      Serial.println("Reset and/or Power On complete");
      stage = 0;
      modemReset = false;
      modemHardResetAndPowerOnPending = false;
    }
  }
}


void modemSetup() {


  if (stage == 0) {
    sendCommand("ATV1", 1000, true);
  }
  if (stage == 1) {
    sendCommand("AT+IPR=115200", 1000, true);  //Baud rate set
  }
  if (stage == 2) {
    sendCommand("ATE0", 1000, true);  //Echo off
  }
  if (stage == 3) {
    sendCommand("AT+CMEE=2", 10000, true);
  }
  if (stage == 4) {
    sendCommand("AT+CPIN?", 10000, true);
  }
  if (stage == 5) {
    if (strstr(response, "SIM PIN") != NULL) {
      sprintf(formattedCommand, "AT+CPIN=\"%s\"", simPIN);
      stage++;
    } else {
      stage = 7;
    }
  }
  if (stage == 6) {
    sendCommand(formattedCommand, 10000, true);
  }
  if (stage == 7) {
    if (modemSoftResetPending == true) {
      sendCommand("AT+CFUN=15", 180000, true);
    } else {
      stage = 10;
    }
  }
  if (stage == 8) {
    delayStartTime = millis();
    stage++;
  }
  if (stage == 9) {
    if (millis() - delayStartTime > 5000) {
      modemSoftResetPending = false;
      stage = 0;
    }
  }
  if (stage == 10) {
    sendCommand("AT+UMNOPROF?", 1000, true);
  }
  if (stage == 11) {
    if (strstr(response, networkProfile) == NULL) {
      stage++;
    } else {
      stage = 16;
    }
  }
  if (stage == 12) {
    sendCommand("AT+CFUN=0", 180000, true);
  }
  if (stage == 13) {
    sprintf(formattedCommand, "AT+UMNOPROF=%s", networkProfile);
    stage++;
  }
  if (stage == 14) {
    sendCommand(formattedCommand, 1000, true);
  }
  if (stage == 15) {
    modemSoftResetPending = true;
    stage = 7;
  }
  if (stage == 16) {
    sendCommand("AT+CGDCONT?", 1000, true);
  }
  if (stage == 17) {
    if (strstr(response, apn) == NULL || strstr(response, "IP") == NULL) {
      stage++;
    } else {
      stage = 22;
    }
  }
  if (stage == 18) {
    sendCommand("AT+CFUN=0", 180000, true);
  }
  if (stage == 19) {
    sprintf(formattedCommand, "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
    stage++;
  }
  if (stage == 20) {
    sendCommand(formattedCommand, 1000, true);
  }
  if (stage == 21) {
    modemSoftResetPending = true;
    stage = 7;
  }
  if (stage == 22) {
    sendCommand("AT+CPSMS?", 1000, true);
  }
  if (stage == 23) {
    if (strstr(response, "+CPSMS:0") == NULL) {
      stage++;
    } else {
      stage = 27;
    }
  }
  if (stage == 24) {
    sendCommand("AT+CFUN=0", 180000, true);
  }
  if (stage == 25) {
    sendCommand("AT+CPSMS=0", 10000, true);
  }
  if (stage == 26) {
    modemSoftResetPending = true;
    stage = 7;
  }
  if (stage == 27) {
    sendCommand("AT+COPS=0", 10000, true);
  }
  if (stage == 28) {
    sendCommand("AT+CEREG=0", 1000, true);
  }
  if (stage == 29) {
    sendCommand("AT+USOCLCFG=1", 1000, true);
  }
  if (stage == 30) {
    sendCommand("AT+UPSV=4", 1000, true);
  }
  if (stage == 31) {
    modemSetupFlag = true;
    stage = 0;
  }
}


void rawHttpPost() {
  if (stage == -1) {
    postStartTime = millis();
    stage++;
  }
  if (stage == 0) {
    sendCommand("ATE0", 1000, true);
  }
  if (stage == 1) {
    sendCommand("AT+CEREG?", 1000, false);
  }
  if (stage == 2) {
    if (strstr(response, ",1") != NULL || strstr(response, ",5") != NULL) {
      stage++;
      Serial.println("Registered");
    } else {
      if (millis() - commandStartTime > 500) {
        stage--;
      }
    }
  }
  if (stage == 3) {
    sendCommand("AT+CGATT?", 1000, false);
  }
  if (stage == 4) {
    if (strstr(response, "+CGATT: 1") != NULL) {
      stage++;
      Serial.println("Attached");
    } else {
      if (millis() - commandStartTime > 500) {
        stage--;
      }
    }
  }
  if (stage == 5) {
    sendCommand("AT+CGACT?", 1000, false);
  }
  if (stage == 6) {
    if (strstr(response, ",1") != NULL) {
      stage++;
      Serial.println("Context Activated");
    } else {
      if (millis() - commandStartTime > 500) {
        stage--;
      }
    }
  }
  if (stage == 7) {
    sendCommand("AT+CESQ", 1000, false);
  }
  if (stage == 8) {
    char *responsePointer;
    int rsrq = 0;


    responsePointer = strstr(response, "+CESQ:");
    if (responsePointer != NULL) {
      int rsrq1 = responsePointer[21] - '0';
      if (responsePointer[22] != ',') {
        int rsrq2 = responsePointer[22] - '0';
        rsrq = (rsrq1 * 10) + rsrq2;
        if (responsePointer[23] != ',') {
          rsrq = 0;
        }
      } else {
        rsrq = rsrq1;
      }
      rsrq_dB = -20 + (.5 * rsrq);


      Serial.print("Signal RSRQ:");
      Serial.print(rsrq_dB);
      Serial.println("dB (closer to zero is better)");
      socket = 0;
      stage++;
    } else {
      stage = 0;
    }
  }
  if (stage == 9) {
    sprintf(formattedCommand, "AT+USOCL=%i,1", socket);
    stage++;
  }
  if (stage == 10) {
    sendCommand(formattedCommand, 150000, true);
  }
  if (stage == 11) {
    socket++;
    if (socket <= 6) {
      stage = stage - 2;
    } else {
      socket = -1;
      stage++;
    }
  }
  if (stage == 12) {
    if (dnsCheckCount >= dnsCheckPeriod) {
      sprintf(formattedCommand, "AT+UDNSRN=0,\"%s\"", thingspeakUrl);
      stage++;
    } else {
      dnsCheckCount++;
      stage = stage + 3;
    }
  }
  if (stage == 13) {
    sendCommand(formattedCommand, 150000, true);
  }
  if (stage == 14) {
    if (responseError) {
      socketErrorCount++;
      stage = 0;
      if (socketErrorCount > 10) {
        modemSetupFlag = false;
        modemSoftResetPending = true;
        socketErrorCount = 0;
      }
    } else {
      socketErrorCount = 0;
      char *responsePointer;
      int pointerPosition = 10;


      responsePointer = strstr(response, "+UDNSRN:");
      if (responsePointer != NULL) {
        int thingspeakIpPosition = 0;
        while (responsePointer[pointerPosition] != '"' && thingspeakIpPosition <= 14) {
          thingspeakIp[thingspeakIpPosition] = responsePointer[pointerPosition];
          thingspeakIpPosition++;
          pointerPosition++;
        }
        thingspeakIp[thingspeakIpPosition] = '\0';
        dnsCheckCount = 0;
        stage++;
      } else {
        stage = 0;
      }
    }
  }
  if (stage == 15) {
    sendCommand("AT+USOCR=6", 120000, true);
  }
  if (stage == 16) {
    if (responseError) {
      modemSetupFlag = false;
      modemSoftResetPending = true;
      stage = 0;
    } else {
      char *responsePointer;


      responsePointer = strstr(response, "+USOCR:");
      if (responsePointer != NULL) {
        socket = responsePointer[8] - '0';
        int securitySetting = 0;
        if (thingspeakPort == 443) {
          securitySetting = 1;
        }
        sprintf(formattedCommand, "AT+USOSEC=%i,%i", socket, securitySetting);
        sprintf(socketCloseIndicator, "+UUSOCL: %i", socket);
        stage++;
      } else {
        stage = 0;
      }
    }
  }
  if (stage == 17) {
    sendCommand(formattedCommand, 1000, false);
  }
  if (stage == 18) {
    sprintf(formattedCommand, "AT+USOCO=%i,\"%s\",%i", socket, thingspeakIp, thingspeakPort);
    stage++;
  }
  if (stage == 19) {
    sendCommand(formattedCommand, 150000, true);
  }
  if (stage == 20) {
    if (responseError) {
      socketErrorCount++;
      stage = 0;
      if (socketErrorCount > 10) {
        modemSetupFlag = false;
        modemSoftResetPending = true;
        socketErrorCount = 0;
      }
    } else {
      socketErrorCount = 0;
      stage++;
    }
  }
  if (stage == 21) {
    char httpBody[500];
    int httpBodySize = 0;
    int formattedHttpSize = 0;


    if (thingspeakCommand) {
      formattedHttpSize = sprintf(formattedHttp, "GET /channels/%s/fields/field%i/last?api_key=%s&headers=false HTTP/1.1\r\n"
                                                 "Host: %s\r\n\r\n",
                                  thingspeakChannelId, thingspeakCommandField, thingspeakReadKey, thingspeakUrl);
    } else {
      int data = 4;
      float batVoltage = analogRead(ADC_BATTERY) * 3.3 / 1023.0 * 1.275;  //1.275 is derived from ADC_BATTERY voltage divider
      char batVolatgeCharArray[6];
      dtostrf(batVoltage, 0, 3, batVolatgeCharArray);


      httpBodySize = sprintf(httpBody, "headers=false&api_key=%s&field1=%s&field2=%i&field3=%i&field4=%i", thingspeakKey, batVolatgeCharArray, data, resetCount, rsrq_dB);


      formattedHttpSize = sprintf(formattedHttp, "POST /update HTTP/1.1\r\n"
                                                 "Host: %s\r\n"
                                                 "Content-Type: application/x-www-form-urlencoded\r\n"
                                                 "Content-Length: %i\r\n\r\n"
                                                 "%s",
                                  thingspeakUrl, httpBodySize, httpBody);
    }


    sprintf(formattedCommand, "AT+USOWR=%i,%i", socket, formattedHttpSize);
    stage++;
  }
  if (stage == 22) {
    sendCommand(formattedCommand, 120000, false);
  }
  if (stage == 23) {
    if (millis() - delayStartTime > 50) {
      stage++;
    }
  }
  if (stage == 24) {
    sendCommand(formattedHttp, 120000, false);
  }
  if (stage == 25) {
    char *responsePointer;
    responsePointer = strstr(response, "+USOWR:");
    if (responsePointer != NULL) {
      if (responsePointer[10] - '0' != 0) {
        loopCount = 0;
        stage++;
      } else {
        stage = 0;
      }
    } else {
      stage = 0;
    }
  }
  if (stage == 26) {
    loopCount++;
    if (loopCount > 1000) {
      loopCount = 0;
      char *responsePointer;
      responsePointer = strstr(response, "+UUSORD:");
      if (responsePointer != NULL) {
        delayStartTime = millis();
        sprintf(formattedCommand, "AT+USORD=%i,1024", socket);
        Serial.println(responsePointer);
        stage++;
      }
      responsePointer = strstr(response, socketCloseIndicator);
      if (responsePointer != NULL) {
        Serial.println(responsePointer);
        stage = 0;
      }
    }


    if (millis() - delayStartTime > 90000) {
      stage = 0;
    }
  }
  if (stage == 27) {
    if (millis() - delayStartTime > 500) {
      stage++;
    }
  }
  if (stage == 28) {
    sendCommand(formattedCommand, 5000, false);
  }
  if (stage == 29) {
    if (strstr(response, "+UUSORD:") != NULL) {
      char *responsePointer;
      char *endPointer;


      responsePointer = strrchr(response, '"');
      endPointer = strrchr(response, '\n');
      responsePosition = responsePosition - ((endPointer - responsePointer) + 1);
      consumeResponseChars = 19;
      continueResponse = true;
      stage = stage - 1;
    } else {
      if (strstr(response, "HTTP/1.1 200") != NULL || strstr(response, "HTTP/1.1 201") != NULL) {
        char *responsePointer;
        char *endPointer;
        int contentLength = 0;
        int receivedContentLength = 0;
        int ones;
        int tens;
        int hundreds;
        int thousands;


        responsePointer = strstr(response, "Content-Length");


        if (responsePointer == NULL) {
          responsePointer = strstr(response, "content-length");  //some server don't Captitalize
        }


        if (responsePointer != NULL) {
          thousands = responsePointer[16] - '0';
          if (responsePointer[17] != '\r') {
            hundreds = responsePointer[17] - '0';
            if (responsePointer[18] != '\r') {
              tens = responsePointer[18] - '0';
              if (responsePointer[19] != '\r') {
                ones = responsePointer[19] - '0';
                contentLength = (1000 * thousands) + (100 * hundreds) + (10 * tens) + ones;
              } else {
                contentLength = (100 * thousands) + (10 * hundreds) + tens;
              }
            } else {
              contentLength = (10 * thousands) + hundreds;
            }
          } else {
            contentLength = thousands;
          }
          Serial.print("Http Header Content Length:");
          Serial.println(contentLength);


          responsePointer = strstr(responsePointer, "\r\n\r\n");


          if (responsePointer != NULL) {
            endPointer = strrchr(responsePointer, '"');
            if (endPointer != NULL) {
              receivedContentLength = endPointer - responsePointer - 4;
              Serial.print("Received Content Length:");
              Serial.println(receivedContentLength);
              if (contentLength == receivedContentLength) {
                if (!thingspeakCommand) {
                  thingspeakCommand = true;
                  stage = 21;
                  httpErrorCount = 0;
                } else {
                  stage++;
                  httpErrorCount = 0;
                  thingspeakCommand = false;
                  if (strstr(responsePointer, "ledon") != NULL) {
                    digitalWrite(LED_BUILTIN, HIGH);
                  }
                  if (strstr(responsePointer, "ledoff") != NULL) {
                    digitalWrite(LED_BUILTIN, LOW);
                  }
                }
              } else {
                stage = 0;
              }
            } else {
              stage = 0;
            }
          } else {
            stage = 0;
          }
        } else {
          stage = 0;
        }
      } else {
        httpErrorCount++;
        stage = 0;
        if (httpErrorCount > 10) {
          stage = 0;
          modemSetupFlag = false;
          modemSoftResetPending = true;
          httpErrorCount = 0;
        }
      }
    }
  }
  if (stage == 30) {
    sprintf(formattedCommand, "AT+USOCL=%i,1", socket);
    stage++;
  }
  if (stage == 31) {
    sendCommand(formattedCommand, 150000, true);
  }
  if (stage == 32) {
    socket = -1;
    if (mcuLowPower) {
      stage = -1;
      unsigned long postDuration = millis() - postStartTime;
      while (postDuration > thingspeakPostInterval) {
        postDuration = postDuration - thingspeakPostInterval;
      }
      Serial.println("MCU sleep until next update...");
      LowPower.sleep(thingspeakPostInterval - postDuration);
    } else {
      stage = 0;
      thingspeakSuccessFlag = true;
    }
  }
}