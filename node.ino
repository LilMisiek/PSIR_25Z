#include <ZsutEthernet.h>
#include <ZsutEthernetUdp.h>
#include "alp.h"

// ==========================================
// KONFIGURACJA SIECI
// ==========================================
// UWAGA: Każdy węzeł musi mieć unikalny MAC!
// Node 0: 00:AA:BB:CC:DE:01
// Node 1: 00:AA:BB:CC:DE:02
// Node 2: 00:AA:BB:CC:DE:03
// Node 3: 00:AA:BB:CC:DE:04
byte mac[] = {0x00, 0xAA, 0xBB, 0xCC, 0xDE, 0x03};  // <-- ZMIEŃ DLA KAŻDEGO WĘZŁA!
unsigned int localPort = ALP_NODE_PORT;

// Adres serwera (zgodnie z konfiguracją emulatora/Linuxa)
ZsutIPAddress serverIP(192, 168, 56, 1); // <-- DOSTOSUJ DO SWOJEJ SIECI
unsigned int serverPort = ALP_SERVER_PORT;

ZsutEthernetUDP Udp;

// ==========================================
// ZMIENNE GLOBALNE I STAN
// ==========================================
// UWAGA: Zmniejszony bufor dla oszczędności RAM (domyślnie 512, ale 256 wystarczy)
uint8_t packetBuffer[256];
uint8_t mySeqNo = 0;
uint8_t myNodeId = 0xFF;

// Konfiguracja obszaru (otrzymana od serwera)
uint16_t area_x_min, area_x_max;
uint16_t area_y_min, area_y_max;
uint8_t step_size = 5;
uint16_t turn_angle = 90;

// Stan Żółwia
float t_x, t_y;
int16_t t_angle;
uint32_t string_pos;
uint32_t total_string_len = 0;

// Stos (dla operacji [ i ])
#define MAX_STACK_DEPTH 20
TurtleStackItem stack[MAX_STACK_DEPTH];
uint16_t stack_depth = 0;

// Lokalna bitmapa ASCII
// UWAGA: Arduino UNO ma tylko 2KB RAM!
// Poprzednio: 40x30 = 1200B + 512B buffer = 1712B (za dużo!)
// Teraz: 20x15 = 300B + 256B buffer = 556B (bezpiecznie)
#define BITMAP_W 20
#define BITMAP_H 15
char bitmap[BITMAP_H][BITMAP_W];
uint32_t total_steps_drawn = 0;

// Flagi stanu
bool isConfigured = false;
bool isDrawing = false;
bool isFinished = false;

// ==========================================
// FUNKCJE POMOCNICZE (ENDIANNESS)
// ==========================================
uint16_t my_htons(uint16_t v) { return (v << 8) | (v >> 8); }
uint16_t my_ntohs(uint16_t v) { return (v << 8) | (v >> 8); }
uint32_t my_htonl(uint32_t v) {
    return ((v & 0xFF000000) >> 24) | ((v & 0x00FF0000) >> 8) |
           ((v & 0x0000FF00) << 8)  | ((v & 0x000000FF) << 24);
}
uint32_t my_ntohl(uint32_t v) { return my_htonl(v); }

// ==========================================
// FUNKCJE BITMAPY
// ==========================================

void clearBitmap() {
    for (int y = 0; y < BITMAP_H; y++) {
        for (int x = 0; x < BITMAP_W; x++) {
            bitmap[y][x] = ' ';
        }
    }
}

void drawPixel(int gx, int gy) {
    int lx = gx - area_x_min;
    int ly = gy - area_y_min;
    
    if (lx >= 0 && lx < BITMAP_W && ly >= 0 && ly < BITMAP_H) {
        bitmap[ly][lx] = '*';
    }
}

void drawLine(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        drawPixel(x0, y0);
        
        if (x0 == x1 && y0 == y1) break;
        
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

// ==========================================
// LOGIKA WYSYŁANIA
// ==========================================

void sendPacket(uint8_t type, void* payload, uint16_t payload_len) {
    memset(packetBuffer, 0, MAX_PACKET_SIZE);
    
    ALPHeader *h = (ALPHeader *)packetBuffer;
    h->type = type;
    h->seq_no = mySeqNo++;
    h->length = my_htons(payload_len);
    
    if (payload_len > 0 && payload != NULL) {
        memcpy(packetBuffer + sizeof(ALPHeader), payload, payload_len);
    }
    
    Udp.beginPacket(serverIP, serverPort);
    Udp.write(packetBuffer, sizeof(ALPHeader) + payload_len);
    Udp.endPacket();
}

void sendRegister() {
    PayloadRegister p;
    p.node_port = my_htons(localPort);
    sendPacket(MSG_REGISTER, &p, sizeof(p));
    Serial.println(F("[NODE] Sent REGISTER"));
}

void requestChunk(uint32_t offset) {
    PayloadRequestChunk p;
    p.offset = my_htonl(offset);
    p.max_len = my_htons(100);
    
    sendPacket(MSG_REQUEST_CHUNK, &p, sizeof(p));
    Serial.print(F("[NODE] Requested chunk from: "));
    Serial.println(offset);
}

void sendHandover(uint8_t dir) {
    uint16_t stack_bytes = stack_depth * sizeof(TurtleStackItem);
    uint16_t total_payload_len = sizeof(PayloadHandover) + stack_bytes;
    
    uint8_t tempBuf[MAX_PACKET_SIZE];
    PayloadHandover *ph = (PayloadHandover *)tempBuf;
    
    ph->target_node_id = 0xFF;
    ph->exit_dir = dir;
    ph->string_pos = my_htonl(string_pos);
    ph->current_x = my_htons((uint16_t)t_x);
    ph->current_y = my_htons((uint16_t)t_y);
    ph->current_angle = my_htons((uint16_t)t_angle);
    ph->stack_depth = my_htons(stack_depth);
    
    if (stack_depth > 0) {
        TurtleStackItem *destStack = (TurtleStackItem *)(tempBuf + sizeof(PayloadHandover));
        for (uint16_t i = 0; i < stack_depth; i++) {
            destStack[i].x = my_htons(stack[i].x);
            destStack[i].y = my_htons(stack[i].y);
            destStack[i].angle = my_htons((uint16_t)stack[i].angle);
        }
    }
    
    sendPacket(MSG_HANDOVER, tempBuf, total_payload_len);
    
    Serial.print(F("[NODE] Sent HANDOVER. Dir: "));
    Serial.print(dir);
    Serial.print(F(" at pos: "));
    Serial.println(string_pos);
    
    isDrawing = false;
}

void sendDone() {
    PayloadDone pd;
    pd.node_id = myNodeId;
    pd.total_steps = my_htonl(total_steps_drawn);
    
    sendPacket(MSG_DONE, &pd, sizeof(pd));
    
    Serial.print(F("[NODE] Sent DONE. Total steps: "));
    Serial.println(total_steps_drawn);
}

// ZMIENIONE: Wysyłanie bitmapy w fragmentach
void sendUpload() {
    // Mniejszy bufor lokalny (256B) - oszczędność RAM
    // Max payload = 256 - 4 (ALPHeader) - 9 (PayloadUpload header) = 243 bajtów
    // Jeden wiersz = BITMAP_W = 20 bajtów
    // Wierszy na pakiet = 243 / 20 = 12 wierszy
    
    const uint16_t local_buffer_size = 256;
    const uint16_t max_pixels_per_packet = local_buffer_size - sizeof(ALPHeader) - PAYLOAD_UPLOAD_HEADER_SIZE;
    uint16_t rows_per_fragment = max_pixels_per_packet / BITMAP_W;
    
    if (rows_per_fragment == 0) rows_per_fragment = 1;
    if (rows_per_fragment > BITMAP_H) rows_per_fragment = BITMAP_H;
    
    uint8_t total_fragments = (BITMAP_H + rows_per_fragment - 1) / rows_per_fragment;
    
    Serial.print(F("[NODE] Sending bitmap in "));
    Serial.print(total_fragments);
    Serial.print(F(" fragments ("));
    Serial.print(rows_per_fragment);
    Serial.println(F(" rows each)"));
    
    // Używamy packetBuffer który już mamy (256 bajtów)
    uint8_t *tempBuf = packetBuffer;
    
    for (uint8_t frag = 0; frag < total_fragments; frag++) {
        uint16_t row_start = frag * rows_per_fragment;
        uint16_t row_count = rows_per_fragment;
        
        if (row_start + row_count > BITMAP_H) {
            row_count = BITMAP_H - row_start;
        }
        
        uint16_t pixels_in_fragment = row_count * BITMAP_W;
        uint16_t payload_size = PAYLOAD_UPLOAD_HEADER_SIZE + pixels_in_fragment;
        
        PayloadUpload *pu = (PayloadUpload *)tempBuf;
        pu->node_id = myNodeId;
        pu->total_width = BITMAP_W;
        pu->total_height = BITMAP_H;
        pu->fragment_id = frag;
        pu->total_fragments = total_fragments;
        pu->row_start = my_htons(row_start);
        pu->row_count = my_htons(row_count);
        
        for (uint16_t y = 0; y < row_count; y++) {
            memcpy(&pu->pixels[y * BITMAP_W], bitmap[row_start + y], BITMAP_W);
        }
        
        sendPacket(MSG_UPLOAD, tempBuf, payload_size);
        
        Serial.print(F("[NODE] Sent fragment "));
        Serial.print(frag + 1);
        Serial.print(F("/"));
        Serial.print(total_fragments);
        Serial.print(F(" (rows "));
        Serial.print(row_start);
        Serial.print(F("-"));
        Serial.print(row_start + row_count - 1);
        Serial.println(F(")"));
        
        delay(50);
    }
    
    Serial.println(F("[NODE] All fragments sent!"));
}

// ==========================================
// LOGIKA RYSOWANIA (INTERPRETER L-SYSTEMU)
// ==========================================

void processChunk(char* data, uint16_t len) {
    Serial.print(F("[NODE] Processing chunk len: "));
    Serial.print(len);
    Serial.print(F(" from pos: "));
    Serial.println(string_pos);

    for (uint16_t i = 0; i < len; i++) {
        char cmd = data[i];

        switch (cmd) {
            case 'F':
            {
                float rad = (float)t_angle * 3.14159265 / 180.0;
                float dx = step_size * cos(rad);
                float dy = step_size * sin(rad);
                
                float new_x = t_x + dx;
                float new_y = t_y + dy;

                uint8_t exit_dir = 0xFF;
                
                if (new_x < area_x_min) {
                    exit_dir = DIR_WEST;
                } else if (new_x >= area_x_max) {
                    exit_dir = DIR_EAST;
                } else if (new_y < area_y_min) {
                    exit_dir = DIR_SOUTH;
                } else if (new_y >= area_y_max) {
                    exit_dir = DIR_NORTH;
                }

                if (exit_dir != 0xFF) {
                    string_pos++;
                    sendHandover(exit_dir);
                    return;
                }

                drawLine((int)t_x, (int)t_y, (int)new_x, (int)new_y);
                total_steps_drawn++;
                
                t_x = new_x;
                t_y = new_y;
                break;
            }
            
            case 'f':
            {
                float rad = (float)t_angle * 3.14159265 / 180.0;
                float dx = step_size * cos(rad);
                float dy = step_size * sin(rad);
                
                float new_x = t_x + dx;
                float new_y = t_y + dy;

                uint8_t exit_dir = 0xFF;
                if (new_x < area_x_min) exit_dir = DIR_WEST;
                else if (new_x >= area_x_max) exit_dir = DIR_EAST;
                else if (new_y < area_y_min) exit_dir = DIR_SOUTH;
                else if (new_y >= area_y_max) exit_dir = DIR_NORTH;

                if (exit_dir != 0xFF) {
                    string_pos++;
                    sendHandover(exit_dir);
                    return;
                }

                t_x = new_x;
                t_y = new_y;
                break;
            }
            
            case '+':
                t_angle = (t_angle + turn_angle) % 360;
                break;
                
            case '-':
                t_angle = (t_angle - turn_angle + 360) % 360;
                break;
                
            case '[':
                if (stack_depth < MAX_STACK_DEPTH) {
                    stack[stack_depth].x = (uint16_t)t_x;
                    stack[stack_depth].y = (uint16_t)t_y;
                    stack[stack_depth].angle = t_angle;
                    stack_depth++;
                } else {
                    Serial.println(F("[WARN] Stack overflow!"));
                }
                break;
                
            case ']':
                if (stack_depth > 0) {
                    stack_depth--;
                    t_x = stack[stack_depth].x;
                    t_y = stack[stack_depth].y;
                    t_angle = stack[stack_depth].angle;
                } else {
                    Serial.println(F("[WARN] Stack underflow!"));
                }
                break;
                
            default:
                break;
        }
        
        string_pos++;
    }
    
    if (total_string_len > 0 && string_pos >= total_string_len) {
        Serial.println(F("[NODE] Reached end of string!"));
        isDrawing = false;
        isFinished = true;
        sendDone();
        sendUpload();
    } else {
        requestChunk(string_pos);
    }
}

// ==========================================
// SETUP & LOOP
// ==========================================

void setup() {
    Serial.begin(115200);
    Serial.println(F("=== L-System Node Starting ==="));

    clearBitmap();

    ZsutEthernet.begin(mac);
    Serial.print(F("My IP: "));
    Serial.println(ZsutEthernet.localIP());

    Udp.begin(localPort);

    delay(1000);
    sendRegister();
}

unsigned long lastRegisterTime = 0;
const unsigned long REGISTER_INTERVAL = 5000;

void loop() {
    if (!isConfigured) {
        unsigned long now = millis();
        if (now - lastRegisterTime > REGISTER_INTERVAL) {
            sendRegister();
            lastRegisterTime = now;
        }
    }
    
    int packetSize = Udp.parsePacket();
    
    if (packetSize > 0) {
        Udp.read(packetBuffer, MAX_PACKET_SIZE);
        
        ALPHeader *h = (ALPHeader *)packetBuffer;
        uint16_t len = my_ntohs(h->length);
        uint8_t *payload = packetBuffer + sizeof(ALPHeader);

        switch (h->type) {
            case MSG_CONFIG: {
                PayloadConfig *cfg = (PayloadConfig *)payload;
                myNodeId = cfg->node_id;
                step_size = cfg->step_size;
                turn_angle = my_ntohs(cfg->angle);
                area_x_min = my_ntohs(cfg->x_min);
                area_x_max = my_ntohs(cfg->x_max);
                area_y_min = my_ntohs(cfg->y_min);
                area_y_max = my_ntohs(cfg->y_max);
                
                isConfigured = true;
                
                Serial.println(F("==== CONFIGURED ===="));
                Serial.print(F("Node ID: ")); Serial.println(myNodeId);
                Serial.print(F("Step: ")); Serial.println(step_size);
                Serial.print(F("Angle: ")); Serial.println(turn_angle);
                Serial.print(F("X: ")); Serial.print(area_x_min); 
                Serial.print(F(" - ")); Serial.println(area_x_max);
                Serial.print(F("Y: ")); Serial.print(area_y_min); 
                Serial.print(F(" - ")); Serial.println(area_y_max);
                break;
            }

            case MSG_START: {
                PayloadStart *s = (PayloadStart *)payload;
                t_x = my_ntohs(s->start_x);
                t_y = my_ntohs(s->start_y);
                t_angle = (int16_t)my_ntohs((uint16_t)s->start_angle);
                string_pos = my_ntohl(s->string_pos);
                stack_depth = 0;
                
                clearBitmap();
                total_steps_drawn = 0;
                isDrawing = true;
                isFinished = false;
                
                Serial.println(F("==== START ===="));
                Serial.print(F("Pos: (")); Serial.print(t_x); 
                Serial.print(F(", ")); Serial.print(t_y); Serial.println(F(")"));
                Serial.print(F("Angle: ")); Serial.println(t_angle);
                Serial.print(F("String pos: ")); Serial.println(string_pos);
                
                requestChunk(string_pos);
                break;
            }

            case MSG_HANDOVER: {
                PayloadHandover *ho = (PayloadHandover *)payload;
                
                if (ho->target_node_id != myNodeId && ho->target_node_id != 0xFF) {
                    Serial.println(F("[INFO] Handover not for me, ignoring."));
                    break;
                }

                t_x = my_ntohs(ho->current_x);
                t_y = my_ntohs(ho->current_y);
                t_angle = (int16_t)my_ntohs((uint16_t)ho->current_angle);
                string_pos = my_ntohl(ho->string_pos);
                stack_depth = my_ntohs(ho->stack_depth);

                if (stack_depth > 0 && stack_depth <= MAX_STACK_DEPTH) {
                    TurtleStackItem *recvStack = (TurtleStackItem *)(payload + sizeof(PayloadHandover));
                    for (uint16_t i = 0; i < stack_depth; i++) {
                        stack[i].x = my_ntohs(recvStack[i].x);
                        stack[i].y = my_ntohs(recvStack[i].y);
                        stack[i].angle = (int16_t)my_ntohs((uint16_t)recvStack[i].angle);
                    }
                }

                isDrawing = true;
                isFinished = false;
                
                Serial.println(F("==== HANDOVER RECEIVED ===="));
                Serial.print(F("Pos: (")); Serial.print(t_x); 
                Serial.print(F(", ")); Serial.print(t_y); Serial.println(F(")"));
                Serial.print(F("Angle: ")); Serial.println(t_angle);
                Serial.print(F("String pos: ")); Serial.println(string_pos);
                Serial.print(F("Stack depth: ")); Serial.println(stack_depth);
                
                requestChunk(string_pos);
                break;
            }

            case MSG_STRING_CHUNK: {
                if (!isDrawing) {
                    Serial.println(F("[WARN] Received chunk but not drawing!"));
                    break;
                }
                
                PayloadStringChunk *sc = (PayloadStringChunk *)payload;
                uint32_t chunk_offset = my_ntohl(sc->offset);
                uint16_t data_len = my_ntohs(sc->data_len);
                total_string_len = my_ntohl(sc->total_len);
                
                Serial.print(F("[CHUNK] offset=")); Serial.print(chunk_offset);
                Serial.print(F(" len=")); Serial.print(data_len);
                Serial.print(F(" total=")); Serial.println(total_string_len);
                
                if (data_len == 0) {
                    Serial.println(F("[NODE] Empty chunk - end of string."));
                    isDrawing = false;
                    isFinished = true;
                    sendDone();
                    sendUpload();
                } else {
                    processChunk(sc->data, data_len);
                }
                break;
            }
            
            case MSG_ACK: {
                PayloadAck *ack = (PayloadAck *)payload;
                Serial.print(F("[ACK] type=")); Serial.print(ack->acked_msg_type);
                Serial.print(F(" seq=")); Serial.println(ack->acked_seq_no);
                break;
            }
            
            case MSG_ERROR: {
                PayloadError *err = (PayloadError *)payload;
                Serial.print(F("[ERROR] code=")); Serial.println(err->error_code);
                break;
            }
            
            default:
                Serial.print(F("[WARN] Unknown message type: 0x"));
                Serial.println(h->type, HEX);
                break;
        }
    }
}
