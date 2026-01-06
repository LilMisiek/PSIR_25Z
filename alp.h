#ifndef ALP_H
#define ALP_H

#include <stdint.h>

/* ==========================================
   KONFIGURACJA PROTOKOŁU (ALP)
   ========================================== */

// Porty i stałe
#define ALP_SERVER_PORT 5000
#define ALP_NODE_PORT   5001
#define MAX_PACKET_SIZE 512  // Ograniczenie bufora Arduino (EBSim)

// Typy wiadomości (Message Types)
#define MSG_REGISTER      0x01
#define MSG_CONFIG        0x02
#define MSG_STRING_CHUNK  0x03
#define MSG_REQUEST_CHUNK 0x04
#define MSG_START         0x05
#define MSG_HANDOVER      0x06
#define MSG_DONE          0x07
#define MSG_UPLOAD        0x08
#define MSG_ACK           0x09
#define MSG_ERROR         0x0A

// Kierunki wyjścia (dla Handover)
#define DIR_NORTH 0
#define DIR_EAST  1
#define DIR_SOUTH 2
#define DIR_WEST  3

// Kody błędów
#define ERR_UNKNOWN_TYPE  0x01
#define ERR_BUFFER_OF     0x02 // Buffer overflow
#define ERR_OUT_OF_BOUNDS 0x03

/* ==========================================
   STRUKTURY DANYCH
   ========================================== */

// Wymuszamy upakowanie struktur co do 1 bajtu (brak paddingu)
#pragma pack(push, 1)

// 1. Nagłówek wspólny dla wszystkich pakietów
typedef struct {
    uint8_t type;       // Typ wiadomości (MSG_*)
    uint8_t seq_no;     // Numer sekwencyjny (do detekcji duplikatów)
    uint16_t length;    // Długość PAYLOADU (bez nagłówka!). Pamiętaj o htons/ntohs!
} ALPHeader;

// 2. Payload: REGISTER (0x01)
// Node -> Server: "Jestem gotowy na tym porcie"
typedef struct {
    uint16_t node_port;
} PayloadRegister;

// 3. Payload: CONFIG (0x02)
// Server -> Node: "To jest twój obszar rysowania"
typedef struct {
    uint8_t node_id;
    uint8_t step_size;  // Długość kreski (d)
    uint16_t angle;     // Kąt obrotu (delta) w stopniach
    uint16_t x_min;     // Granice obszaru węzła
    uint16_t x_max;
    uint16_t y_min;
    uint16_t y_max;
} PayloadConfig;

// 4. Payload: REQUEST_CHUNK (0x04)
// Node -> Server: "Daj mi kawałek stringa od tej pozycji"
typedef struct {
    uint32_t offset;    // Od którego znaku zacząć (NETWORK BYTE ORDER!)
    uint16_t max_len;   // Ile znaków max mogę przyjąć
} PayloadRequestChunk;

// 5. Payload: STRING_CHUNK (0x03)
// Server -> Node: "Oto kawałek stringa L-systemu"
typedef struct {
    uint32_t offset;    // Pozycja startowa tego kawałka w całym stringu
    uint16_t data_len;  // Rzeczywista długość danych w tablicy data
    uint32_t total_len; // Całkowita długość stringa (żeby węzeł wiedział kiedy koniec)
    char data[];        // Elastyczna tablica (Flexible Array Member)
} PayloadStringChunk;

// 6. Payload: START (0x05)
// Server -> Node: "Zacznij rysować od tego stanu"
typedef struct {
    uint16_t start_x;
    uint16_t start_y;
    int16_t start_angle; // Może być ujemny
    uint32_t string_pos; // Pozycja w stringu (indeks) - NETWORK BYTE ORDER!
} PayloadStart;

// Pomocnicza struktura dla stosu (potrzebna w Handover)
typedef struct {
    uint16_t x;
    uint16_t y;
    int16_t angle;
} TurtleStackItem;

// 7. Payload: HANDOVER (0x06)
// Node -> Server: "Żółw wyszedł poza mój obszar, przekaż go dalej"
// Server -> Node (Target): "Przejmij żółwia"
typedef struct {
    uint8_t target_node_id; // Kto ma przejąć (lub 0xFF jeśli nieznany)
    uint8_t exit_dir;       // DIR_NORTH, DIR_EAST itd.
    uint32_t string_pos;    // Gdzie przerwaliśmy w stringu (NETWORK BYTE ORDER!)
    uint16_t current_x;
    uint16_t current_y;
    int16_t current_angle;
    uint16_t stack_depth;   // Ile elementów jest na stosie
    TurtleStackItem stack[];// Zrzut stosu (dynamiczna wielkość)
} PayloadHandover;

// 8. Payload: DONE (0x07)
// Node -> Server: "Skończyłem rysować (koniec stringa)"
typedef struct {
    uint8_t node_id;
    uint32_t total_steps; // Statystyka: ile kroków narysowano (NETWORK BYTE ORDER!)
} PayloadDone;

// 9. Payload: UPLOAD (0x08) - Z FRAGMENTACJĄ
// Node -> Server: "Oto fragment wyrenderowanej bitmapy (ASCII)"
// Bitmapa jest dzielona na fragmenty wierszowe, bo cała nie mieści się w MAX_PACKET_SIZE
typedef struct {
    uint8_t node_id;
    uint8_t total_width;    // Całkowita szerokość bitmapy węzła
    uint8_t total_height;   // Całkowita wysokość bitmapy węzła
    uint8_t fragment_id;    // Numer fragmentu (0, 1, 2, ...)
    uint8_t total_fragments;// Całkowita liczba fragmentów
    uint16_t row_start;     // Od którego wiersza zaczyna się ten fragment (NETWORK BYTE ORDER!)
    uint16_t row_count;     // Ile wierszy w tym fragmencie (NETWORK BYTE ORDER!)
    char pixels[];          // Dane bitmapy (ASCII), wiersz po wierszu
} PayloadUpload;

// 10. Payload: ACK (0x09)
// Potwierdzenie otrzymania (prosta niezawodność)
typedef struct {
    uint8_t acked_msg_type; // Typ potwierdzanej wiadomości
    uint8_t acked_seq_no;   // Numer sekwencyjny potwierdzanej wiadomości
} PayloadAck;

// 11. Payload: ERROR (0x0A)
typedef struct {
    uint8_t error_code;
    char message[];     // Opcjonalny opis tekstowy
} PayloadError;

#pragma pack(pop)

/* ==========================================
   STAŁE POMOCNICZE DLA FRAGMENTACJI UPLOAD
   ========================================== */

// Rozmiar nagłówka PayloadUpload (bez pixels[])
#define PAYLOAD_UPLOAD_HEADER_SIZE 9

// Max bajtów na piksele w jednym pakiecie UPLOAD
// = MAX_PACKET_SIZE - sizeof(ALPHeader) - PAYLOAD_UPLOAD_HEADER_SIZE
// = 512 - 4 - 9 = 499 bajtów
#define MAX_UPLOAD_PIXELS_PER_PACKET (MAX_PACKET_SIZE - sizeof(ALPHeader) - PAYLOAD_UPLOAD_HEADER_SIZE)

#endif // ALP_H
