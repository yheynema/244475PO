/*
  Démontrateur pour la mesure de hauteur par méthode ultrasonique, sauvegarde sur carte SD.
  Communication des Objets 4.0

  Y.Heynemand - Hiver 2024

  Inspirations:
  - SD:  https://randomnerdtutorials.com/esp32-data-logging-temperature-to-microsd-card/
  - DS18B20: https://lastminuteengineers.com/ds18b20-arduino-tutorial/
  - Ultrason: https://lastminuteengineers.com/arduino-sr04-ultrasonic-sensor-tutorial/
  - NTP: (de mon cru et qqe détails dans les pages de doc du esp32 de Espressif)

  Prochaine version: publier sur MQTT la distance afin de la récupérer via un client web et la lib Paho en JS
  29 février 2024: Polling asynchrone, par interruption plutot que pulseIn bloquant...
*/

#include <Ecran.h>;
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <TimeLib.h>
#include <esp_wifi.h>
#include <esp_sntp.h>               //Pour l'accès aux fct et constantes NTP
#include <Average.h>
#include <MQTT.h>                //Lib by Joël Gähwiler : https://github.com/256dpi/arduino-mqtt  v2.5.1  (lwmqtt)
#include <ArduinoJson.h>
#include <LDR.h>
#include "secrets.h"

#define _VERSION "4.1.1"
#define BASENOMFICHIER "/dt0.csv"
#define sysID "007"
#define REDPIN 4
#define GREENPIN 2

//--- QQE constantes
//Broches utilisées:
const int trigPin    = 16; //label RX2
const int echoPin    = 17; //label TX2
const int pinLDR     = 34; //Avec le wifi on est restreint au ADC1, pins 32 à 39
const int oneWirePin = 15; // One Wire bus pin for DS18B20 sensor
const int SD_CS_Pin  = 5;  // Define CS pin for the SD card module

const float alpha =  0.25;  //Filter for LDR reading

const char* fileHead = "id,timestamp,systemID,temperature,echoTime,distance\n";
const uint8_t dataTblSz = 4;
const uint8_t maxMissed = 4;
//MQTT server:
//IPAddress MQTTserver(192, 168, 122, 253);  //Local Mosquito server
const char mqttHostProvider[]=MQTT_BROKER;  //adresse du serveur MQTT (FQDN)
const char mqttDevice[]="JSNSR04TYh";
const char mqttUser[] = MQTT_USER;
const char mqttSubscribeTo[]="244475AL/lab01yhsub";  //Nom du canal (topic) d'abonnement (NE PAS commencer par "/")
const char mqttPublishTo[]="244475AL/lab01yhpub";  //Nom du canal (topic) de publication (NE PAS commencer par "/")
const char mqttToken[] = MQTT_TOKEN;

//--- Objets ----------------------------------------
Ecran monEcran;
File  monFichier;
// Setup a oneWire instance to communicate with a OneWire device
OneWire oneWire(oneWirePin);
// Pass our oneWire reference to Dallas Temperature sensor 
DallasTemperature sensors(&oneWire);

LDR monLDR(pinLDR);

Average<uint32_t> aveDelay(dataTblSz);
Average<uint32_t> aveDistance(dataTblSz);
Average<float> aveTemp(dataTblSz);

//Pour envoi brut à MQTT - Shiftr.io
WiFiClient espClient;
MQTTClient clientMQTT(512);

//Pour envoi à TB - technophys-tb.claurendeau.qc.ca
WiFiClient espClientTB;
MQTTClient clientTB(512);
//---------------------------------------------------

const char* ssid       = WIFI_SSID;
const char* password   = WIFI_PASS;

//Canal de publication TB:
const char TBPublishTo[]="v1/devices/me/telemetry";  //Nom du canal (topic) de publication

//Configuration du RTC via SNTP:
const char* ntpServer = "ca.pool.ntp.org";
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 0;

const byte myMAC[6] = MY_MAC_ADDR; //Cette séquence sera fournie par l'enseignant

//Liste en francais. Note: Septembre a été tronqué car il entre pas dans l'écran OLED avec chiffre de 2 positions.
const char* mois[12] = {"Janvier","Février","Mars","Avril","Mai","Juin","Juillet","Août","Septembr","Octobre","Novembre","Décembre"};
const char* jour[7] = {"Dimanche","Lundi","Mardi","Mercredi","Jeudi","Vendredi","Samedi"};

//Contrôle des boucles non-bloquantes (attente Wifi, RTC):
const int maxLoopCount = 60;

//Constantes de calculs et de validité des données:
const float baseSpeed = 331.3; //Vitesse du son à 0C
const float baseConstant = 0.606; // variation en fct de la temperature: (m/s)/C
const uint32_t minDuration = 1450; // temps pour faire 25cm: 0.25m/ 331.2m/s * 1E6 = 754us -> 725us * 2
const uint16_t minDistance = 200; // 200mm, en vrai serait 25cm
const uint16_t maxDistance = 4000; //4 mètres

//Variables décompte
uint32_t sentMsgCount = 0;   //Compteur de message de sortie au MQTT
uint32_t recvMsgCount = 0;   //Compteur de message reçus via MQTT

//Variables des minuteries:
bool pretEffacer = false;
uint32_t SDWriteTimer=0;        //Minuterie de commande d'écriture sur la carte SD
const uint32_t SDWriteDelay = 30000L;

uint32_t affichageCapteurTimer = 0;  //Minuterie d'affichage
const uint32_t affichageCapteurDelay = 1000L; //soit 3x le temps de waitBetweenSamples...

uint32_t lectureCapteurTimer = 0;     //Minuterie de lecture du capteur de hauteur
const int waitBetweenSamples = 35;   //ms entre chaque mesure

uint32_t sendMQTTTimer = 0;           //Minuterie pour envoyer la donnée à MQTT
const uint32_t sendMQTTDelay = 2000; //Délais d'envoi à MQTT;

//Variables d'état:
uint16_t fileEntryCount = 0;     //Compteur d'entrées dans le fichier de données de la carte SD
bool SDAvailable = false;        //État de la détection d'une carte SD
bool RTCAvailable = false;       //État du Wifi... et du RTC
bool MQTTAvailable = false;      //État du service MQTT
bool tempSensorAvail = false;    //État du capteur de température
bool distanceAvailable = false;  //État du capteur de mesure de hauteur
uint16_t sampleCounter = 0;      //Compteur d'échantillonnage

volatile uint32_t chronoDepart = 0;
volatile uint32_t chronoArret = 0;
volatile byte chronoStatus =0;
volatile bool chronoAtteint = false;

int pollingRate =0;
uint32_t lastPollTime = micros();

//--- Fonctions du sketche ------------------------------------------------

void callback(String &topic, String &payload) {
  Serial.println("incoming("+String(recvMsgCount)+"): " + topic + " - " + payload);

  // Ici on devrait traiter les commandes de configuration?

}

bool reconnectMQTT() {
  bool retCode = false;
  int retryCount = 3;
  // Loop until we're reconnected
  while (!clientMQTT.connected() && retryCount>0) {
    Serial.print("Attempting MQTT connection...");
    retryCount--;
    // Attempt to connect
    //if (clientMQTT.connect(mqttDevice)) {   //Local Mosquito
    if (clientMQTT.connect(mqttDevice, mqttUser, mqttToken)) {  //Shiftr.io
      Serial.println("connected");
      MQTTAvailable = true;
      clientMQTT.subscribe(mqttSubscribeTo);
      retCode = true;
    } else {
      MQTTAvailable = false;
      Serial.print("failed, rc=");
      Serial.print(clientMQTT.returnCode());
      Serial.println(" try again in 5 seconds");
      // Wait 2 seconds before retrying
      delay(2000);
    }
  }
  return retCode;
}

bool sendMeasureData2MQTT(const char* mqttPubQ) {
  bool retCode = false;

  time_t myTime;
  time(&myTime);

  JsonDocument doc;

  doc["firmware"] = _VERSION;
  doc["delay"] = aveDelay.mean();
  doc["distance"] = aveDistance.mean();
  doc["temperature"] = aveTemp.mean();
  doc["ldrvalue"] = monLDR.getPct();
  doc["msgcount"] = sampleCounter;
  doc["nblogsd"] = fileEntryCount;
  doc["mqttmsg"] = sentMsgCount;
  doc["timestamp"] = myTime;
  doc["uptime"] = millis()/1000;

  char buffer[250];

  int dataSize = serializeJson(doc, buffer);

  if (clientMQTT.connected()) {
    if (clientMQTT.publish(mqttPubQ, buffer)) {
      //Incrémente le compteur de message envoyé. Affiche l'information sur le moniteur série:
      sentMsgCount++;
      Serial.println("> Publication msg("+String(sentMsgCount)+") au sujet "+String(mqttPubQ)+" de "+String(dataSize)+" octets.");
      retCode = true;
    } else {
      //En cas d'erreur d'envoi, on affiche les détails:
      Serial.println("> ERREUR publication: "+String(clientMQTT.lastError()));
      if (clientMQTT.connected())
        Serial.println("> clientMQTT connecté");
      else
        Serial.println("> clientMQTT NON-connecté");
      Serial.println("> Msg ("+String(dataSize)+") longueur est: "+buffer);
    }//Fin if-then-else publication MQTT
  } else {
    Serial.println("> Ne peut procéder: clientMQTT non-connecté!");
  }

  // Manage to send data to TB
  Serial.print("Tentative TB: ");
  if (clientTB.connect(TB_DEVICE,TB_TOKEN)) {  //TB selon secret.h
      Serial.print("Connecté à TB");
      //Forger le data en réutilisant l'objet doc
      doc.clear();
      doc["delay"] = aveDelay.mean();
      doc["distance"] = aveDistance.mean();
      doc["temperature"] = aveTemp.mean();
      doc["ldrvalue"] = monLDR.getPct();

      int dataSize = serializeJson(doc, buffer);

      if (dataSize > 10) {
        if (clientTB.connected()) {
          if (clientTB.publish(TBPublishTo, buffer)) {
            Serial.print(" > Data sent to TB successfully");
            clientTB.disconnect();
            retCode = true;
          } else {
            Serial.print(" > Error with data sent to TB");
          }
        } else Serial.print(" > ERROR: not connected!?!");
      } else Serial.print(" > ERROR: incorrect size: "+String(dataSize));
  } else {
    Serial.print(" > ERREUR: Ne peut connecter à TB");
  }
  Serial.println(".");
  return retCode;
}


bool initSD(const char * myFile) {
  bool retCode = false;

  digitalWrite(REDPIN,HIGH); //Indiquer qu'une operation est en cours et non-réussie

  //initialisation de la SD
  if(!SD.begin(SD_CS_Pin)) {
    Serial.println("> ne peut pas communiquer avec la SD");
    return false;
  }

  Serial.println("> SD en position");
  
  if (SD.exists(myFile)) {
    Serial.println("> fichier existe");
    File FH = SD.open(myFile,FILE_READ);
    if (FH) {
      fileEntryCount = lireNbligne(FH);
      FH.close();
      retCode = true;
    }    
  } else {
    //file does not exist
    Serial.println("> fichier n'existe pas");
    File FH = SD.open(myFile,FILE_WRITE);
    if (FH) {
      Serial.println("> init du fichier");
      if (FH.print(fileHead)) {
        fileEntryCount = 1;
        FH.close();
        retCode = true;
      }
    } else Serial.println("> ne peut ecrire dans le fichier");   
  }
  if (retCode) { 
    digitalWrite(REDPIN,LOW);
  }
  return retCode;
}

// Write the sensor readings on the SD card
bool logSDCard(int readingID, float temperature, int travelTime, int distance) {
  bool retCode = false;
  time_t myTime;
  time(&myTime);

  String dataMessage = String(readingID) + "," + "sysID_"+String(sysID)+","+ 
  String(myTime) + "," + String(temperature,1) + "," +
  String(travelTime) + ","+String(distance)+","+"\r\n";
  Serial.print("Save data: ");
  Serial.println(dataMessage);
  if (appendFile(SD, BASENOMFICHIER, dataMessage.c_str())) {
    fileEntryCount++;
    retCode = true;
  }
  return retCode;
}

// Append data to the SD card (DON'T MODIFY THIS FUNCTION)
bool appendFile(fs::FS &fs, const char * path, const char * message) {
  bool retCode = false;

  Serial.printf("Appending to file: %s\n", path);

  File file = fs.open(path, FILE_APPEND);
  if(!file) {
    Serial.println("Failed to open file for appending");
    return retCode;
  }
  if(file.print(message)) {
    Serial.println("Message appended");
    retCode = true;
  } else {
    Serial.println("Append failed");
  }
  file.close();
  return retCode;
}

uint16_t lireNbligne(File datFile) {
  uint16_t linecount = 0;
  char inputChar;
  
  if (datFile) {
    while (datFile.available()) {
      inputChar = datFile.read();
      if(inputChar == '\n') linecount++;
    }
  }
  return linecount;
}

bool initRTC() {
  bool retCode = false;

  digitalWrite(REDPIN,HIGH); //Indiquer qu'une operation est en cours et non-réussie

  if (String(WIFI_SSID).length() > 3) {
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_mac(WIFI_IF_STA, &myMAC[0]);
    
    if(strlen(password) > 0)
      WiFi.begin(ssid, password);
    else
      WiFi.begin(ssid);
    int loopCount = 0;
    char tmpBuff[22]={0};
    while (WiFi.status() != WL_CONNECTED && loopCount<maxLoopCount) {
        if (loopCount%20 == 0) {
          monEcran.effacer(3);
          tmpBuff[0]='.';
          tmpBuff[1]=0;
        }
        strcat(tmpBuff,".");
        monEcran.ecrire(tmpBuff,3);
        monEcran.refresh();
        Serial.print(".");
        loopCount++;
        delay(500);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[WiFi] WiFi is connected!");
      Serial.print("[WiFi] IP address: ");
      Serial.println(WiFi.localIP().toString());
      //init and get the time
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      int loopCount = 0;
      char tmpBuff[22]={0};
      while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && loopCount<maxLoopCount) {
        if (loopCount%20 == 0) {
          monEcran.effacer(3);
          tmpBuff[0]='*';
          tmpBuff[1]=0;
        }
        strcat(tmpBuff,"*");
        monEcran.ecrire(tmpBuff,3);
        monEcran.refresh();
        Serial.print("*");
        loopCount++;
        delay(500);
      }
      if (loopCount<maxLoopCount) retCode = true;

    }
  }
  monEcran.effacer(3);
  monEcran.refresh();
  if (retCode) digitalWrite(REDPIN,LOW);
  return retCode;
}

bool lectureTemperature() {
  bool retCode = false;
  sensors.requestTemperatures(); 
  float temp = sensors.getTempCByIndex(0);
  if (temp > -40.0 && temp < 40.0) {
    aveTemp.push(temp);
    retCode = true;
  }
  return retCode;
}

float appliqueFacteurCorrection(float value, float pente, float ordOrig) {
  return value*pente+ordOrig;
}

void IRAM_ATTR stopChrono() {
  if (chronoStatus == 1) {
    chronoDepart = micros();
    chronoStatus = 2;
    return;
  }
  if (chronoStatus == 2) {
      chronoArret = micros();
      chronoStatus = 3;
  }
}

//Lecture du capteur ultrason de maniere non-bloquante
void lancerUltrason() {
  // Clears the trigPin
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  // Sets the trigPin on HIGH state for 10 micro seconds
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);  //Was 10
  digitalWrite(trigPin, LOW);
  chronoStatus = 1;
  attachInterrupt(echoPin, stopChrono, CHANGE);
}

bool calculDistance() {
  bool retCode = false;
  float avgTemp = 0.0;
  avgTemp = aveTemp.mean();
  
  float soundSpeed = (baseSpeed+avgTemp*baseConstant)/1E3; // en mm/us https://en.wikipedia.org/wiki/Speed_of_sound

  if (chronoArret > chronoDepart) {
    uint32_t duration = chronoArret - chronoDepart;
    if (duration > minDuration) {
      aveDelay.push(duration);
      float distancemm = duration * soundSpeed/2;
      if (distancemm > minDistance && distancemm < maxDistance) {
        float valeurCorrigee = appliqueFacteurCorrection(distancemm,1.0,0.0);
        aveDistance.push(valeurCorrigee);
        retCode = true;
      }
    }
  }
  return retCode;
}


void setup() {
  Serial.begin(115200);
  while (!Serial);

  monEcran.begin();
  monEcran.print("UltraLog v"+String(_VERSION));
  monEcran.refresh();

  pinMode(REDPIN,OUTPUT);
  pinMode(GREENPIN,OUTPUT);

  pinMode(trigPin, OUTPUT); // Sets the trigPin as an Output
  pinMode(echoPin, INPUT); // Sets the echoPin as an Input

  digitalWrite(REDPIN,LOW);
  digitalWrite(GREENPIN,LOW);

  if (!initSD(BASENOMFICHIER)) {
    SDAvailable = false;
    Serial.println("> ERREUR - carte SD non disponible");
    monEcran.ecrire("Erreur SD",2,1);
  } else {
    SDAvailable = true;
    Serial.println("> OK - carte SD et fichier disponibles");
    monEcran.ecrire("SD Ok",2,1);
  }
  monEcran.refresh();

  //clientMQTT.begin(MQTTserver, espClient); //Local Mosquito
  clientMQTT.begin(mqttHostProvider,espClient);               //Shiftr.io
  clientMQTT.onMessage(callback);

  clientTB.begin(TB_HOST, espClientTB);

  if (!initRTC()){
    Serial.println("> ERREUR - time sync impossible");
    monEcran.ecrire("Erreur NTP",3,1);
    RTCAvailable = false;
  } else {
    RTCAvailable = true;
    reconnectMQTT();
    Serial.println("> OK - RTC configuré");
    monEcran.ecrire("RTC Ok",3,1);
  }
  monEcran.refresh();

  monLDR.setAlpha(alpha);

  sensors.begin();
  sensors.requestTemperatures(); 
  float temperature = sensors.getTempCByIndex(0);
  if (temperature > -40.0 && temperature < 40.0) tempSensorAvail = true; else tempSensorAvail = false;
}


void loop() {

  monLDR.update();

  if (millis() > SDWriteTimer+SDWriteDelay) {  //l'heure de logger le data SI la SD est initialisée
    SDWriteTimer=millis();
    digitalWrite(GREENPIN,HIGH);
    uint16_t delais = (uint16_t)(aveDelay.mean()+0.5);
    uint16_t distance = (uint16_t)(aveDistance.mean()+0.5);
    if (SDAvailable) {
      if (logSDCard(fileEntryCount,aveTemp.mean(),delais,distance)) {
        monEcran.effacer(4);
        monEcran.ecrire(String("Log Ok:"+String(fileEntryCount)).c_str(),4);
        monEcran.refresh();
        pretEffacer = true;
      }
    }
    digitalWrite(GREENPIN,LOW);
  }

  //1/4 du temps d'attendre apres écriture, effacer le msg d'état d'écriture sur la SD:
  if (millis()>SDWriteTimer+SDWriteDelay/4 && pretEffacer) {
    monEcran.effacer(4);
    monEcran.refresh();
    pretEffacer = false;
  }

  //Minuterie de lecture du capteur, basic: au 100ms, non-bloquant
  if (millis() > lectureCapteurTimer +waitBetweenSamples) {
    lectureCapteurTimer = millis();
    digitalWrite(REDPIN,HIGH);
    float avgTemp = 0.0;
    
    if (chronoStatus ==0)
      lancerUltrason();
      
  }

  if (chronoStatus == 3) {
    detachInterrupt(echoPin);
    chronoStatus = 0;
    if (lectureTemperature())
      if (calculDistance()) {
        sampleCounter++;
        distanceAvailable=true;
      } else distanceAvailable=false;
    digitalWrite(REDPIN,LOW);
    pollingRate = 1E6L/(micros()-lastPollTime);
    lastPollTime = micros();
  }

  //Minuterie d'affichage des information sur le OLED:
  if (millis() > affichageCapteurTimer + affichageCapteurDelay) {
    affichageCapteurTimer = millis();  
    monEcran.effacer(5);
    monEcran.effacer(6);
    Serial.println("Lpct="+String(monLDR.getPct(),1));
    if (distanceAvailable) 
      monEcran.ecrire("dst:"+String(aveDistance.mean(),0)+"mm dT="+String(aveDelay.mean()/1000.0,2) +"ms",5);
    else
      monEcran.ecrire("dst: ---mm dT= ---ms",5);
    monEcran.ecrire("T="+String(aveTemp.mean(),1)+"C samp:"+String(sampleCounter),6);
    monEcran.ecrire("Rate="+String(pollingRate),7,1);
    //Afficher l'heure?
    monEcran.refresh();
    
  }

  //Minuterie d'envoi des informations vers le MQTT:
  if (millis() > sendMQTTTimer+sendMQTTDelay) {
    sendMQTTTimer = millis();
    if (RTCAvailable) reconnectMQTT(); //Si Wifi etait ok, check la connexion avec MQTT, va updater MQTTAvailable
    if (MQTTAvailable) {
      if (sendMeasureData2MQTT(mqttPublishTo)) { 
        Serial.println(" > Log to MQTT completed");
      } else Serial.println(" > Log to MQTT NOT completed!!");
    }
  }
  
}
