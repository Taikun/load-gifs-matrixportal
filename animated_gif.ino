/*********************************************************************
  EJEMPLO FLEXIBLE PARA MOSTRAR GIFS EN UNA MATRIZ RGB CON ADAFRUIT
  MATRIXPORTAL, USANDO UNA ESTRUCTURA MÁS MODULAR Y CONTROL NO BLOQUEANTE.
  
  - Se escanea la carpeta /gifs y se almacena la lista en un vector.
  - Se reproduce cada GIF cuatro ciclos, luego pasa al siguiente.
  - Tras el último GIF, se muestra la animación "Compu LAB" estilo Pipboy.
  - Uso de control de frames con millis() en vez de delay() para no
    bloquear el loop y permitir manejo de botones u otras tareas.
*********************************************************************/

#define _VARIANT_MATRIXPORTAL_M4_  // Forzar configuración si es necesario

#include <Adafruit_Protomatter.h>
#include <Adafruit_SPIFlash.h>
#include <Adafruit_TinyUSB.h>
#include <AnimatedGIF.h>
#include <SPI.h>
#include <SdFat.h>
#include <vector>
#include <cassert>

// =================== CONFIGURACIÓN BÁSICA ===================
#define WIDTH  64
#define HEIGHT 64
const char *GIF_DIR = "/gifs"; // Carpeta donde buscar los GIF

// Cuántos ciclos (repeticiones) de cada GIF antes de pasar al siguiente
#define GIF_CYCLES 4

// ---------------- FLASH / FILESYSTEM ----------------
#if defined(ARDUINO_ARCH_ESP32)
  static Adafruit_FlashTransport_ESP32 flashTransport;
#elif defined(EXTERNAL_FLASH_USE_QSPI)
  Adafruit_FlashTransport_QSPI flashTransport;
#elif defined(EXTERNAL_FLASH_USE_SPI)
  Adafruit_FlashTransport_SPI flashTransport(EXTERNAL_FLASH_USE_CS,
                                             EXTERNAL_FLASH_USE_SPI);
#else
  #error No QSPI/SPI flash are defined in your board variant.h!
#endif

Adafruit_SPIFlash flash(&flashTransport);
FatFileSystem      filesys;
Adafruit_USBD_MSC  usb_msc;
static bool        msc_changed = true; // Para detectar cambios en el FS

// ---------------- PROTOMATTER (MATRIZ) ----------------
#if defined(_VARIANT_MATRIXPORTAL_M4_)
  uint8_t rgbPins[]  = {7, 8, 9, 10, 11, 12};
  uint8_t addrPins[] = {17, 18, 19, 20, 21};
  uint8_t clockPin   = 14;
  uint8_t latchPin   = 15;
  uint8_t oePin      = 16;
  #define BACK_BUTTON 2
  #define NEXT_BUTTON 3
#elif defined(ARDUINO_ADAFRUIT_MATRIXPORTAL_ESP32S3)
  uint8_t rgbPins[]  = {42, 41, 40, 38, 39, 37};
  uint8_t addrPins[] = {45, 36, 48, 35, 21};
  uint8_t clockPin   = 2;
  uint8_t latchPin   = 47;
  uint8_t oePin      = 14;
  #define BACK_BUTTON 6
  #define NEXT_BUTTON 7
#endif

#if HEIGHT == 16
  #define NUM_ADDR_PINS 3
#elif HEIGHT == 32
  #define NUM_ADDR_PINS 4
#elif HEIGHT == 64
  #define NUM_ADDR_PINS 5
#endif

Adafruit_Protomatter matrix(
  WIDTH, 6, 1, rgbPins, NUM_ADDR_PINS, addrPins,
  clockPin, latchPin, oePin, true);

// --------------- ESTRUCTURAS / CLASES MODULARES ---------------

// ---------- 1) FileManager: escanea y guarda lista de GIFs ----
std::vector<String> gifList;  // Única declaración del vector

void scanGifs(const char *path, std::vector<String> &list) {
  list.clear();
  File dir = filesys.open(path);
  if (!dir) {
    Serial.println("No se pudo abrir el directorio de GIFs.");
    return;
  }
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break; // no hay más archivos
    if (!entry.isDirectory()) {
      char filename[256];
      entry.getName(filename, sizeof(filename) - 1);
      // Ignoramos archivos que empiecen con "._" (basura de Mac) y revisamos la extensión
      if (strncmp(filename, "._", 2) != 0) {
        char *extension = strrchr(filename, '.');
        if (extension && !strcasecmp(&extension[1], "GIF")) {
          list.push_back(String("/") + String(path) + "/" + String(filename));
        }
      }
    }
    entry.close();
  }
  dir.close();

  Serial.print("Encontrados ");
  Serial.print(list.size());
  Serial.println(" GIF(s).");
}

// ---------- 2) GifPlayer: manejo de la librería AnimatedGIF ---
class GifPlayer {
public:
  AnimatedGIF   gif;       // Objeto de la librería
  File          gifFile;   // Archivo abierto
  bool          isOpen;    // Indica si hay un GIF abierto
  uint32_t      lastFrameTime;  // Para control no bloqueante
  int32_t       nextFrameDelay; // ms a esperar para siguiente frame
  int16_t       xPos, yPos;     // Para centrar
  int           currentCycle;   // Cuántas repeticiones se han completado
  bool          cycleCounted;   // Evitar contar el mismo ciclo varias veces

  GifPlayer() : isOpen(false), lastFrameTime(0), nextFrameDelay(0),
                xPos(0), yPos(0), currentCycle(0), cycleCounted(false) {}

  // Funciones requeridas por AnimatedGIF
  static void * openCallback(const char *filename, int32_t *pSize) {
    Serial.print("GIFOpenFile: ");
    Serial.println(filename);
    GifPlayer *player = &instance(); // Instancia singleton
    player->gifFile = filesys.open(filename);
    if (!player->gifFile) {
      Serial.println("Error abriendo archivo");
      return NULL;
    }
    *pSize = player->gifFile.size();
    return (void *)&player->gifFile;
  }

  static void closeCallback(void *pHandle) {
    File *f = static_cast<File *>(pHandle);
    if (f) {
      f->close();
    }
  }

  static int32_t readCallback(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
    File *f = static_cast<File *>(pFile->fHandle);
    if (!f) return 0;
    if ((pFile->iSize - pFile->iPos) < iLen) {
      iLen = pFile->iSize - pFile->iPos;
    }
    int32_t bytesRead = f->read(pBuf, iLen);
    pFile->iPos = f->position();
    return bytesRead;
  }

  static int32_t seekCallback(GIFFILE *pFile, int32_t iPosition) {
    File *f = static_cast<File *>(pFile->fHandle);
    if (!f) return -1;
    f->seek(iPosition);
    pFile->iPos = f->position();
    return pFile->iPos;
  }

  static void drawCallback(GIFDRAW *pDraw) {
    GifPlayer &player = instance();
    int16_t screenY = player.yPos + pDraw->iY + pDraw->y;
    if (screenY < 0 || screenY >= matrix.height()) return;

    uint8_t  *s = pDraw->pPixels;
    uint16_t *usPalette = pDraw->pPalette;
    uint16_t tempLine[320];

    if (pDraw->ucHasTransparency) {
      uint8_t ucTransparent = pDraw->ucTransparent;
      uint8_t *pEnd = s + pDraw->iWidth;
      int x = 0;
      while (x < pDraw->iWidth) {
        int count = 0;
        while ((s < pEnd) && (*s != ucTransparent)) {
          tempLine[count++] = usPalette[*s++];
        }
        if (count) {
          memcpy(matrix.getBuffer() + screenY * matrix.width() + player.xPos + pDraw->iX + x,
                 tempLine, count * 2);
          x += count;
        }
        count = 0;
        while ((s < pEnd) && (*s == ucTransparent)) {
          s++;
          count++;
        }
        x += count;
      }
    } else {
      for (int x = 0; x < pDraw->iWidth; x++) {
        tempLine[x] = usPalette[*s++];
      }
      memcpy(matrix.getBuffer() + screenY * matrix.width() + player.xPos + pDraw->iX,
             tempLine, pDraw->iWidth * 2);
    }
  }

  // Inicializar la librería (llamar en setup)
  void begin() {
    gif.begin(LITTLE_ENDIAN_PIXELS);
  }

  // Abrir un GIF dado su nombre
  bool openGif(const char *filename) {
    closeGif();
    isOpen = gif.open(filename, openCallback, closeCallback,
                      readCallback, seekCallback, drawCallback);
    if (!isOpen) {
      Serial.println("Fallo abriendo el GIF");
      return false;
    }
    xPos = (matrix.width()  - gif.getCanvasWidth())  / 2;
    yPos = (matrix.height() - gif.getCanvasHeight()) / 2;
    currentCycle   = 0;
    cycleCounted   = false;
    lastFrameTime  = millis();
    nextFrameDelay = 0;
    // Limpiamos la pantalla al abrir el GIF
    matrix.fillScreen(0);
    matrix.show();
    return true;
  }

  // Cerrar el GIF y limpiar la pantalla
  void closeGif() {
    if (isOpen) {
      gif.close();
      isOpen = false;
      gifFile.close();
      matrix.fillScreen(0);
      matrix.show();
    }
  }

  // Reproducir el frame actual (no bloqueante)
  bool playFrame() {
    if (!isOpen) return false;

    unsigned long now = millis();
    if ((long)(now - lastFrameTime) >= nextFrameDelay) {
      nextFrameDelay = gif.playFrame(true, NULL);
      // Aplica un factor de ralentización (ajusta según tu preferencia)
      nextFrameDelay *= 3;  
      matrix.show();
      lastFrameTime = now;

      if (nextFrameDelay < 0) {
        return false;
      }

      if (gifFile && gifFile.position() < 10 && !cycleCounted) {
        currentCycle++;
        cycleCounted = true;
        Serial.print("Ciclo completo: ");
        Serial.println(currentCycle);
      } else if (gifFile && gifFile.position() >= 10) {
        cycleCounted = false;
      }

      if (currentCycle >= GIF_CYCLES) {
        return false;
      }
    }
    return true;
  }

  static GifPlayer &instance() {
    static GifPlayer single;
    return single;
  }
};

// ---------- 3) UI: Mostrar texto animado "Compu LAB" ------------
void showAnimatedText() {
  // Limpia la pantalla
  matrix.fillScreen(0);
  matrix.show();
  
  // Color Pipboy: verde brillante (RGB565)
  uint16_t pipColor = 0x07E0;
  
  // Dibuja un borde doble para el efecto Pipboy
  for (int i = 0; i < 2; i++) {
    matrix.drawRect(i, i, matrix.width() - 2 * i, matrix.height() - 2 * i, pipColor);
  }
  
  // Configura el tamaño y el color del texto
  matrix.setTextSize(2);
  matrix.setTextColor(pipColor);
  
  // Texto a mostrar
  const char *line1 = "Compu";
  const char *line2 = "LAB";
  
  // Calcula el ancho aproximado de cada línea (el ancho de cada carácter es ~6px a tamaño 1, 12px a tamaño 2)
  int compuWidth = 5 * 12; // Aproximadamente 60px para "Compu"
  int labWidth   = 3 * 12; // Aproximadamente 36px para "LAB"
  
  // Calcula la posición final para centrar cada línea
  int targetXCompu = (matrix.width() - compuWidth) / 2;
  int targetXLab   = (matrix.width() - labWidth) / 2;
  
  // Altura de línea (aprox. 16px para tamaño 2)
  int lineHeight = 16;
  // Calcula las posiciones verticales (se puede ajustar para un look más dinámico)
  int yCompu = (matrix.height() - (lineHeight * 2)) / 2 - 2;
  int yLab   = yCompu + lineHeight;
  
  // Valor inicial para la animación (comenzamos fuera de pantalla a la izquierda)
  float startX = -matrix.width();
  
  // Efecto "slide in": interpolamos un parámetro t de 0 a 1
  for (float t = 0.0; t <= 1.0; t += 0.05) {
    int currentXCompu = startX * (1.0 - t) + targetXCompu * t;
    int currentXLab   = startX * (1.0 - t) + targetXLab * t;
    
    matrix.fillScreen(0);
    // Redibuja el borde
    for (int i = 0; i < 2; i++) {
      matrix.drawRect(i, i, matrix.width() - 2 * i, matrix.height() - 2 * i, pipColor);
    }
    // Dibuja el texto en su posición actual
    matrix.setCursor(currentXCompu, yCompu);
    matrix.print(line1);
    matrix.setCursor(currentXLab, yLab);
    matrix.print(line2);
    matrix.show();
    delay(30);
  }
  
  // Mantén el mensaje final centrado durante 5 segundos
  unsigned long holdTime = millis();
  while (millis() - holdTime < 5000) {
    matrix.show();
    delay(100);
  }
  
  // Limpia la pantalla al finalizar
  matrix.fillScreen(0);
  matrix.show();
}




// --------------- VARIABLES GLOBALES PARA EL LOOP ---------------
GifPlayer &gifPlayer = GifPlayer::instance();
int16_t gifIndex = -1;        // Índice actual en gifList
int8_t  gifIncrement = 1;     // Dirección (+1 = siguiente, -1 = anterior)

// --------------- CALLBACKS MSC (mass storage) ---------------
int32_t msc_read_cb(uint32_t lba, void *buffer, uint32_t bufsize) {
  return flash.readBlocks(lba, (uint8_t *)buffer, bufsize / 512) ? bufsize : -1;
}
int32_t msc_write_cb(uint32_t lba, uint8_t *buffer, uint32_t bufsize) {
  digitalWrite(LED_BUILTIN, HIGH);
  return flash.writeBlocks(lba, buffer, bufsize / 512) ? bufsize : -1;
}
void msc_flush_cb(void) {
  flash.syncBlocks();
  filesys.cacheClear();
  digitalWrite(LED_BUILTIN, LOW);
  msc_changed = true;
}

// ========================== SETUP ===========================
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
#if defined(BACK_BUTTON)
  pinMode(BACK_BUTTON, INPUT_PULLUP);
#endif
#if defined(NEXT_BUTTON)
  pinMode(NEXT_BUTTON, INPUT_PULLUP);
#endif

  flash.begin();
  usb_msc.setID("Adafruit", "External Flash", "1.0");
  usb_msc.setCapacity(flash.pageSize() * flash.numPages() / 512, 512);
  usb_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);
  usb_msc.setUnitReady(true);
  usb_msc.begin();
  filesys.begin(&flash);

  Serial.begin(115200);
  // while(!Serial);

  ProtomatterStatus status = matrix.begin();
  Serial.print("Protomatter status: ");
  Serial.println((int)status);
  matrix.fillScreen(0);
  matrix.show();

  gifPlayer.begin();

  scanGifs(GIF_DIR, gifList);

  gifIndex = -1;
  gifIncrement = 1;
}

// =========================== LOOP ===========================
void loop() {
  if (msc_changed) {
    msc_changed = false;
    scanGifs(GIF_DIR, gifList);
    gifIncrement = 1;
    return;
  }

#if defined(BACK_BUTTON)
  if (!digitalRead(BACK_BUTTON)) {
    gifIncrement = -1;
    while(!digitalRead(BACK_BUTTON));
  }
#endif
#if defined(NEXT_BUTTON)
  if (!digitalRead(NEXT_BUTTON)) {
    gifIncrement = 1;
    while(!digitalRead(NEXT_BUTTON));
  }
#endif

  if (gifIncrement != 0) {
    if (gifPlayer.isOpen) {
      gifPlayer.closeGif();
    }
    // Limpiar la pantalla antes de abrir un nuevo GIF
    matrix.fillScreen(0);
    matrix.show();

    if (gifList.empty()) {
      matrix.fillScreen(0);
      matrix.show();
      return;
    }

    gifIndex += gifIncrement;
    if (gifIndex >= (int)gifList.size()) {
      showAnimatedText();
      gifIndex = 0;
    } else if (gifIndex < 0) {
      gifIndex = gifList.size() - 1;
    }
    Serial.print("Abriendo: ");
    Serial.println(gifList[gifIndex]);

    if (gifPlayer.openGif(gifList[gifIndex].c_str())) {
      Serial.println("GIF abierto correctamente");
    }
    gifIncrement = 0;
  }
  else {
    if (gifPlayer.isOpen) {
      bool playing = gifPlayer.playFrame();
      if (!playing) {
        gifIncrement = 1;
      }
    }
  }
}
