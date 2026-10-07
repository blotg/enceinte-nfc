#include "Arduino.h"
#include "Audio.h"
#include "SD.h"
#include "FS.h"

//#include <PN532_HSU.h>
//#include <PN532.h>
#include <Adafruit_PN532.h>

// Digital I/O used
#define SD_CS          5
#define SPI_MOSI      16
#define SPI_MISO      15
#define SPI_SCK       12
#define I2S_DOUT       9
#define I2S_BCLK       3
#define I2S_LRC        1
#define I2C_SDA        6
#define I2C_SCL        7
#define RX2_PIN       19
#define TX2_PIN       20

Audio audio;

Adafruit_PN532 nfc(-1, &Serial2);


void setup() {
  // === COMMUNICATION PC POUR DEBUG ===
  Serial.begin(115200);
  
  // === CONFIG SD ===
  pinMode(SD_CS, OUTPUT);      digitalWrite(SD_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  SD.begin(SD_CS);

  // === CONFIG AUDIO MAX98357A ===
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(5); // default 0...21
  
  // === CONFIG LECTEUR NFC PN532 ===
  Serial.println("Initialisation de la connection avec la carte PN53x");
  bool PN532Connecte = false;
  uint32_t versiondata;
  Serial.print("Tentative de connection avec la carte PN53x ...");
  while(!PN532Connecte) {
    delay(100);
    nfc.begin();
    versiondata = nfc.getFirmwareVersion();
    if (! versiondata) {
      Serial.println("échec");
    } else {
      PN532Connecte = true;
    }
    
  }
  Serial.print("trouvé : PN5"); Serial.println((versiondata>>24) & 0xFF, HEX);
  Serial.print("Firmware ver. "); Serial.print((versiondata>>16) & 0xFF, DEC);
  Serial.print('.'); Serial.println((versiondata>>8) & 0xFF, DEC);
  //nfc.setPassiveActivationRetries(0xFF);  // Tentatives infinies
  //nfc.SAMConfig();
  Serial.println("Attente d'une carte ISO14443A Card ...");
}

uint8_t lastUid[7];
uint8_t lastUidLength = 0;
uint8_t uid[7];
uint8_t uidLength;
const unsigned long CARD_TIMEOUT = 50;  // ms
const unsigned long PN532_RESET_PERIOD = 2000; // ms
unsigned long last_reset = 0;

std::vector<char*>  v_audioContent;

void loop(){
  audio.loop();

  if (millis()-last_reset > PN532_RESET_PERIOD) {
    //Serial.println("Reset du PN532");
    nfc.begin();
    last_reset = millis();
  }
  
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, CARD_TIMEOUT)) {
    if (!compareUid(uid, uidLength, lastUid, lastUidLength)) {// si c'est une nouvelle carte
      Serial.print("UID Value: "); Serial.println(uidToString(uid, uidLength)); // affichage de son uid
      for (uint8_t i = 0; i < uidLength; i++) lastUid[i] = uid[i];
      lastUidLength = uidLength;
      populatePlaylist(uidToString(uid, uidLength).c_str());
      nfc.begin();
      nfc.SAMConfig();
    }
  }
  vTaskDelay(1);
}



String uidToString(uint8_t* uid, uint8_t uidLength) {
  String uidStr = "/";
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 0x10) uidStr += "0";  // Ajoute un 0 pour les valeurs < 0x10
    uidStr += String(uid[i], HEX);
  }
  return uidStr;
}

bool compareUid(uint8_t* uid1, uint8_t len1, uint8_t* uid2, uint8_t len2) {
  if (len1 != len2) return false;
  for (int i = 0; i < len1; i++) {
    if (uid1[i] != uid2[i]) return false;
  }
  return true;
}

void populatePlaylist(const char* uid){
  
  vector_clear_and_shrink(v_audioContent);
  File root = SD.open(uid);
  if(!root){
      Serial.println("Failed to open directory");
      return;
  }
  if(!root.isDirectory()){
      Serial.println("Not a directory");
      return;
  }

  File file = root.openNextFile();
  while(file){
      if(!file.isDirectory()){
          v_audioContent.insert(v_audioContent.begin(), strdup(file.path()));
      }
      file = root.openNextFile();
  }
  Serial.printf("num files %i", v_audioContent.size());
  Serial.println();
  root.close();
  file.close();

  if(v_audioContent.size() > 0){
      const char* s = (const char*)v_audioContent[v_audioContent.size() -1];
      Serial.printf("playing %s\n", s);
      audio.connecttoFS(SD, s);
      v_audioContent.pop_back();
  }
}

void vector_clear_and_shrink(std::vector<char*>&vec){
    uint size = vec.size();
    for (int i = 0; i < size; i++) {
        if(vec[i]){
            free(vec[i]);
            vec[i] = NULL;
        }
    }
    vec.clear();
    vec.shrink_to_fit();
}

void audio_eof_mp3(const char *info){
  Serial.print("eof_mp3     ");Serial.println(info);
  if(v_audioContent.size() > 0){ 
    const char* s = (const char*)v_audioContent[v_audioContent.size() - 1];
    Serial.printf("playing %s\n", s);
    audio.connecttoFS(SD, s);
    v_audioContent.pop_back();
  } else {
    Serial.println("Playlist terminée");
  }
}
