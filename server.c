#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include "alp.h"

// Konfiguracja
#define MAX_NODES 4
#define CANVAS_WIDTH 40      // 2 węzły × 20 pikseli
#define CANVAS_HEIGHT 30     // 2 węzły × 15 pikseli
#define NODE_BITMAP_W 20     // Szerokość bitmapy węzła
#define NODE_BITMAP_H 15     // Wysokość bitmapy węzła
#define L_SYSTEM_MAX_LEN 100000

// Struktura L-systemu
#define MAX_RULES 26         // A-Z
#define MAX_RULE_LEN 256

typedef struct {
    char axiom[256];
    char rules[MAX_RULES][MAX_RULE_LEN];  // rules['F'-'A'] = "F+F-F"
    int angle;
    int iterations;
} LSystemDef;

// Struktura przechowująca stan węzła
typedef struct {
    int id;
    int active;
    int finished;
    int fragments_received;  // ZMIENIONE: licznik fragmentów zamiast bool uploaded
    int total_fragments;     // DODANE: oczekiwana liczba fragmentów
    struct sockaddr_in addr;
    uint16_t x_min, x_max;
    uint16_t y_min, y_max;
} NodeInfo;

// Zmienne globalne
int sockfd;
NodeInfo nodes[MAX_NODES];
int registered_count = 0;
char l_system_string[L_SYSTEM_MAX_LEN];
uint32_t l_system_len = 0;

// Globalna bitmapa do składania
char final_bitmap[CANVAS_HEIGHT][CANVAS_WIDTH];

// Statystyki
int total_handovers = 0;
int messages_sent = 0;
int messages_received = 0;

// Definicja L-systemu (wczytana z pliku)
LSystemDef lsystem;

// Wczytaj L-system z pliku
// Format pliku:
//   axiom: F
//   angle: 90
//   iterations: 3
//   rule: F -> F+F-F-F+F
//   rule: X -> XX
int load_lsystem(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("Cannot open L-system file");
        return -1;
    }
    
    // Domyślne wartości
    strcpy(lsystem.axiom, "F");
    lsystem.angle = 90;
    lsystem.iterations = 2;
    memset(lsystem.rules, 0, sizeof(lsystem.rules));
    
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Usuń newline
        line[strcspn(line, "\r\n")] = 0;
        
        // Pomiń puste linie i komentarze
        if (line[0] == '\0' || line[0] == '#') continue;
        
        if (strncmp(line, "axiom:", 6) == 0) {
            // axiom: F+F+F+F
            char *val = line + 6;
            while (*val == ' ') val++;
            strncpy(lsystem.axiom, val, sizeof(lsystem.axiom) - 1);
            printf("[LSYS] Axiom: %s\n", lsystem.axiom);
        }
        else if (strncmp(line, "angle:", 6) == 0) {
            // angle: 90
            lsystem.angle = atoi(line + 6);
            printf("[LSYS] Angle: %d\n", lsystem.angle);
        }
        else if (strncmp(line, "iterations:", 11) == 0) {
            // iterations: 3
            lsystem.iterations = atoi(line + 11);
            printf("[LSYS] Iterations: %d\n", lsystem.iterations);
        }
        else if (strncmp(line, "rule:", 5) == 0) {
            // rule: F -> F+F-F-F+F
            char *ptr = line + 5;
            while (*ptr == ' ') ptr++;
            
            char symbol = *ptr;
            if (symbol < 'A' || symbol > 'Z') {
                printf("[WARN] Invalid rule symbol: %c\n", symbol);
                continue;
            }
            
            // Znajdź "->"
            char *arrow = strstr(ptr, "->");
            if (!arrow) {
                printf("[WARN] Invalid rule format (no ->): %s\n", line);
                continue;
            }
            
            char *replacement = arrow + 2;
            while (*replacement == ' ') replacement++;
            
            int idx = symbol - 'A';
            strncpy(lsystem.rules[idx], replacement, MAX_RULE_LEN - 1);
            printf("[LSYS] Rule: %c -> %s\n", symbol, lsystem.rules[idx]);
        }
    }
    
    fclose(f);
    return 0;
}

// Generuj string L-systemu na podstawie wczytanej definicji
void generate_lsystem() {
    char temp[L_SYSTEM_MAX_LEN];
    strncpy(l_system_string, lsystem.axiom, L_SYSTEM_MAX_LEN - 1);
    
    printf("[SERVER] Generating L-system: axiom='%s', iterations=%d, angle=%d\n",
           lsystem.axiom, lsystem.iterations, lsystem.angle);
    
    for (int iter = 0; iter < lsystem.iterations; iter++) {
        memset(temp, 0, L_SYSTEM_MAX_LEN);
        char *src = l_system_string;
        char *dst = temp;
        size_t remaining = L_SYSTEM_MAX_LEN - 1;
        
        while (*src && remaining > 0) {
            int idx = *src - 'A';
            
            // Sprawdź czy jest reguła dla tego symbolu
            if (idx >= 0 && idx < MAX_RULES && lsystem.rules[idx][0] != '\0') {
                size_t rule_len = strlen(lsystem.rules[idx]);
                if (rule_len <= remaining) {
                    memcpy(dst, lsystem.rules[idx], rule_len);
                    dst += rule_len;
                    remaining -= rule_len;
                } else {
                    break;  // Brak miejsca
                }
            } else {
                // Brak reguły - kopiuj symbol bez zmian
                *dst++ = *src;
                remaining--;
            }
            src++;
        }
        *dst = '\0';
        strcpy(l_system_string, temp);
        
        printf("[SERVER] After iteration %d: length=%zu\n", iter + 1, strlen(l_system_string));
    }
    
    l_system_len = strlen(l_system_string);
    printf("[SERVER] L-System generated. Final length: %u symbols.\n", l_system_len);
    
    // Pokaż początek stringa (debug)
    if (l_system_len > 50) {
        printf("[SERVER] String preview: %.50s...\n", l_system_string);
    } else {
        printf("[SERVER] String: %s\n", l_system_string);
    }
}

// Funkcja pomocnicza do wysyłania pakietów
void send_alp_packet(struct sockaddr_in *target, uint8_t type, void *payload, uint16_t payload_len) {
    uint8_t buffer[MAX_PACKET_SIZE];
    ALPHeader *header = (ALPHeader *)buffer;
    
    static uint8_t seq_counter = 0;
    header->type = type;
    header->seq_no = seq_counter++;
    header->length = htons(payload_len);
    
    if (payload && payload_len > 0) {
        memcpy(buffer + sizeof(ALPHeader), payload, payload_len);
    }
    
    sendto(sockfd, buffer, sizeof(ALPHeader) + payload_len, 0,
           (struct sockaddr *)target, sizeof(struct sockaddr_in));
    
    messages_sent++;
}

// Znajdź ID węzła po adresie IP/Port
int find_node_index(struct sockaddr_in *addr) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (nodes[i].active &&
            nodes[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            nodes[i].addr.sin_port == addr->sin_port) {
            return i;
        }
    }
    return -1;
}

// Logika podziału ekranu (2x2)
// Layout:
//   Node 0 (TL) | Node 1 (TR)    <- y >= mid_y (góra)
//   Node 2 (BL) | Node 3 (BR)    <- y < mid_y  (dół)
void assign_region(int node_idx) {
    uint16_t w = CANVAS_WIDTH / 2;
    uint16_t h = CANVAS_HEIGHT / 2;
    
    nodes[node_idx].x_min = (node_idx % 2) * w;
    nodes[node_idx].x_max = nodes[node_idx].x_min + w;
    
    if (node_idx < 2) {
        nodes[node_idx].y_min = h;
        nodes[node_idx].y_max = CANVAS_HEIGHT;
    } else {
        nodes[node_idx].y_min = 0;
        nodes[node_idx].y_max = h;
    }
    
    printf("[SERVER] Node %d assigned region: X[%d-%d] Y[%d-%d]\n", 
           node_idx, nodes[node_idx].x_min, nodes[node_idx].x_max,
           nodes[node_idx].y_min, nodes[node_idx].y_max);
}

// Wyświetl złożoną bitmapę ASCII
void print_final_bitmap() {
    printf("\n========== FINAL RENDER ==========\n");
    // Drukujemy od góry (wysokie Y) do dołu (niskie Y)
    for (int y = CANVAS_HEIGHT - 1; y >= 0; y--) {
        for (int x = 0; x < CANVAS_WIDTH; x++) {
            putchar(final_bitmap[y][x]);
        }
        putchar('\n');
    }
    printf("===================================\n");
}

// Sprawdź czy wszystkie węzły zakończyły i przesłały wszystkie fragmenty
void check_completion() {
    int all_done = 1;
    for (int i = 0; i < registered_count; i++) {
        if (nodes[i].fragments_received < nodes[i].total_fragments) {
            all_done = 0;
            break;
        }
    }
    
    if (all_done && registered_count == MAX_NODES) {
        printf("\n[SERVER] All nodes finished!\n");
        printf("[STATS] Total handovers: %d\n", total_handovers);
        printf("[STATS] Messages sent: %d, received: %d\n", messages_sent, messages_received);
        print_final_bitmap();
    }
}

int main(int argc, char *argv[]) {
    struct sockaddr_in server_addr, client_addr;
    uint8_t buffer[MAX_PACKET_SIZE];
    socklen_t addr_len = sizeof(client_addr);

    // Sprawdź argumenty
    if (argc < 2) {
        printf("Usage: %s <lsystem_file>\n", argv[0]);
        printf("Example: %s koch.txt\n", argv[0]);
        printf("\nL-system file format:\n");
        printf("  axiom: F\n");
        printf("  angle: 90\n");
        printf("  iterations: 3\n");
        printf("  rule: F -> F+F-F-F+F\n");
        return 1;
    }

    // Inicjalizacja bitmapy spacjami
    memset(final_bitmap, ' ', sizeof(final_bitmap));

    // 1. Wczytaj i wygeneruj L-System z pliku
    if (load_lsystem(argv[1]) < 0) {
        return 1;
    }
    generate_lsystem();
    
    if (l_system_len == 0) {
        printf("[ERROR] L-system string is empty!\n");
        return 1;
    }

    // 2. Setup Gniazda
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(ALP_SERVER_PORT);

    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    printf("[SERVER] Listening on port %d...\n", ALP_SERVER_PORT);
    printf("[SERVER] Canvas size: %dx%d\n", CANVAS_WIDTH, CANVAS_HEIGHT);

    // 3. Pętla główna
    while (1) {
        ssize_t n = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0, 
                             (struct sockaddr *)&client_addr, &addr_len);
        
        if (n < (ssize_t)sizeof(ALPHeader)) continue;

        messages_received++;

        ALPHeader *header = (ALPHeader *)buffer;
        uint16_t payload_len = ntohs(header->length);
        uint8_t *payload_ptr = buffer + sizeof(ALPHeader);

        int node_idx = find_node_index(&client_addr);

        switch (header->type) {
            case MSG_REGISTER: {
                if (registered_count >= MAX_NODES) {
                    printf("[WARN] Ignored REGISTER: Max nodes reached.\n");
                    break;
                }
                
                if (node_idx == -1) {
                    node_idx = registered_count++;
                    nodes[node_idx].active = 1;
                    nodes[node_idx].finished = 0;
                    nodes[node_idx].fragments_received = 0;
                    // Oblicz oczekiwaną liczbę fragmentów na podstawie znanego rozmiaru
                    // Node wysyła 20x15 bitmap, bufor 256B, max 243B na piksele
                    // 243 / 20 = 12 wierszy na fragment
                    // ceil(15 / 12) = 2 fragmenty
                    nodes[node_idx].total_fragments = (NODE_BITMAP_H + 11) / 12;  // = 2
                    nodes[node_idx].id = node_idx;
                    nodes[node_idx].addr = client_addr;
                    assign_region(node_idx);
                    
                    printf("[SERVER] Node %d expected fragments: %d\n", 
                           node_idx, nodes[node_idx].total_fragments);
                }

                // Wyślij CONFIG
                PayloadConfig cfg;
                cfg.node_id = node_idx;
                cfg.step_size = 2;
                cfg.angle = htons(lsystem.angle);  // Kąt z pliku L-systemu
                cfg.x_min = htons(nodes[node_idx].x_min);
                cfg.x_max = htons(nodes[node_idx].x_max);
                cfg.y_min = htons(nodes[node_idx].y_min);
                cfg.y_max = htons(nodes[node_idx].y_max);

                send_alp_packet(&client_addr, MSG_CONFIG, &cfg, sizeof(cfg));
                printf("[SERVER] Sent CONFIG to Node %d\n", node_idx);

                // Jeśli wszystkie węzły zarejestrowane, wyślij START do Node 2
                if (registered_count == MAX_NODES) {
                    printf("[SERVER] All %d nodes registered. Starting render...\n", MAX_NODES);
                    
                    int start_node = 2;
                    PayloadStart start;
                    start.start_x = htons(nodes[start_node].x_min + 5);
                    start.start_y = htons(nodes[start_node].y_min + 5);
                    start.start_angle = htons(0);
                    start.string_pos = htonl(0);
                    
                    send_alp_packet(&nodes[start_node].addr, MSG_START, &start, sizeof(start));
                    printf("[SERVER] Sent START to Node %d at position (%d, %d)\n", 
                           start_node, nodes[start_node].x_min + 5, nodes[start_node].y_min + 5);
                }
                break;
            }

            case MSG_REQUEST_CHUNK: {
                if (node_idx == -1) break;
                
                PayloadRequestChunk *req = (PayloadRequestChunk *)payload_ptr;
                uint32_t offset = ntohl(req->offset);
                uint16_t req_len = ntohs(req->max_len);

                uint8_t chunk_buf[MAX_PACKET_SIZE];
                PayloadStringChunk *chunk = (PayloadStringChunk *)chunk_buf;
                
                if (offset >= l_system_len) {
                    chunk->offset = htonl(offset);
                    chunk->data_len = htons(0);
                    chunk->total_len = htonl(l_system_len);
                    send_alp_packet(&client_addr, MSG_STRING_CHUNK, chunk, sizeof(PayloadStringChunk));
                    printf("[SERVER] Sent empty chunk to Node %d (end of string)\n", node_idx);
                    break;
                }

                uint16_t actual_len = req_len;
                if (offset + actual_len > l_system_len) {
                    actual_len = l_system_len - offset;
                }
                
                if (actual_len > MAX_PACKET_SIZE - sizeof(ALPHeader) - sizeof(PayloadStringChunk)) {
                    actual_len = MAX_PACKET_SIZE - sizeof(ALPHeader) - sizeof(PayloadStringChunk);
                }

                chunk->offset = htonl(offset);
                chunk->data_len = htons(actual_len);
                chunk->total_len = htonl(l_system_len);
                
                if (actual_len > 0) {
                    memcpy(chunk->data, &l_system_string[offset], actual_len);
                }

                send_alp_packet(&client_addr, MSG_STRING_CHUNK, chunk, 
                               sizeof(PayloadStringChunk) + actual_len);
                
                if (offset % 1000 == 0 || offset + actual_len >= l_system_len) {
                    printf("[SERVER] Sent chunk to Node %d: offset=%u, len=%u/%u\n", 
                           node_idx, offset, actual_len, l_system_len);
                }
                break;
            }

            case MSG_HANDOVER: {
                PayloadHandover *ho = (PayloadHandover *)payload_ptr;
                uint8_t exit_dir = ho->exit_dir;
                int source_id = node_idx;
                int target_id = -1;

                switch (exit_dir) {
                    case DIR_NORTH:
                        target_id = (source_id == 2) ? 0 : (source_id == 3) ? 1 : -1;
                        break;
                    case DIR_SOUTH:
                        target_id = (source_id == 0) ? 2 : (source_id == 1) ? 3 : -1;
                        break;
                    case DIR_EAST:
                        target_id = (source_id == 0) ? 1 : (source_id == 2) ? 3 : -1;
                        break;
                    case DIR_WEST:
                        target_id = (source_id == 1) ? 0 : (source_id == 3) ? 2 : -1;
                        break;
                }

                if (target_id != -1 && nodes[target_id].active) {
                    total_handovers++;
                    printf("[SERVER] HANDOVER #%d: Node %d -> Node %d (Dir: %d, Pos: %u)\n", 
                           total_handovers, source_id, target_id, exit_dir, ntohl(ho->string_pos));
                    
                    ho->target_node_id = target_id;
                    
                    send_alp_packet(&nodes[target_id].addr, MSG_HANDOVER, payload_ptr, payload_len);
                } else {
                    printf("[SERVER] Turtle exited canvas bounds (Source: %d, Dir: %d). Marking as done.\n", 
                           source_id, exit_dir);
                    nodes[source_id].finished = 1;
                }
                break;
            }
            
            case MSG_DONE: {
                if (node_idx == -1) break;
                
                PayloadDone *done = (PayloadDone *)payload_ptr;
                nodes[node_idx].finished = 1;
                printf("[SERVER] Node %d finished. Total steps: %u\n", 
                       node_idx, ntohl(done->total_steps));
                break;
            }
            
            case MSG_UPLOAD: {
                if (node_idx == -1) break;
                
                PayloadUpload *up = (PayloadUpload *)payload_ptr;
                uint8_t total_width = up->total_width;
                uint8_t total_height = up->total_height;
                uint8_t fragment_id = up->fragment_id;
                uint8_t total_fragments = up->total_fragments;
                uint16_t row_start = ntohs(up->row_start);
                uint16_t row_count = ntohs(up->row_count);
                
                printf("[SERVER] UPLOAD from Node %d: fragment %d/%d, rows %d-%d (%dx%d total)\n", 
                       node_idx, fragment_id + 1, total_fragments, 
                       row_start, row_start + row_count - 1,
                       total_width, total_height);
                
                // Zapisz oczekiwaną liczbę fragmentów
                if (nodes[node_idx].total_fragments == 0) {
                    nodes[node_idx].total_fragments = total_fragments;
                }
                
                // Wstaw fragment bitmapy do globalnej bitmapy
                uint16_t base_x = nodes[node_idx].x_min;
                uint16_t base_y = nodes[node_idx].y_min;
                
                for (uint16_t y = 0; y < row_count; y++) {
                    uint16_t global_y = base_y + row_start + y;
                    if (global_y >= CANVAS_HEIGHT) continue;
                    
                    for (uint16_t x = 0; x < total_width; x++) {
                        uint16_t global_x = base_x + x;
                        if (global_x >= CANVAS_WIDTH) continue;
                        
                        char pixel = up->pixels[y * total_width + x];
                        if (pixel != ' ') {
                            final_bitmap[global_y][global_x] = pixel;
                        }
                    }
                }
                
                nodes[node_idx].fragments_received++;
                
                printf("[SERVER] Node %d: received %d/%d fragments\n",
                       node_idx, nodes[node_idx].fragments_received, nodes[node_idx].total_fragments);
                
                check_completion();
                break;
            }
            
            case MSG_ACK: {
                break;
            }
            
            case MSG_ERROR: {
                PayloadError *err = (PayloadError *)payload_ptr;
                printf("[SERVER] ERROR from Node %d: code=%d\n", node_idx, err->error_code);
                break;
            }
        }
    }

    close(sockfd);
    return 0;
}
