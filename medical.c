#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>    // Needed for billing date/time, expiry check, invoice ID
#include <ctype.h>   // For isdigit, isxdigit, isspace, tolower
#include <errno.h>   // For checking file errors
#include <limits.h>  // For LONG_MAX, LONG_MIN, INT_MAX, INT_MIN
#ifdef _WIN32
#include <process.h> // For _getpid() on Windows
#define getpid _getpid // Define getpid for Windows
#else
#include <unistd.h>  // For getpid() on POSIX
#endif

#define STOCK_FILE "stock.csv"
#define SALES_FILE "sales.csv"
#define TEMP_STOCK_FILE_UPDATE "stock_temp_update.csv" // Used by processUpdateStock
#define TEMP_STOCK_FILE_BILLING "stock_temp_billing.csv" // Used by processBilling
#define MAX_BILL_ITEMS 50 // Maximum number of different medicines per bill
#define HASH_TABLE_SIZE 101 // Prime number for better distribution

// --- Data Structures ---
struct medicine
{
    char name[40];
    int mcode;
    char s_name[50];
    long long s_contact;
    float price;
    int quantity;
    int year;
    int month;
    int day;
};

struct sale_record // Used by billing and reporting
{
    char invoice_id[30]; // Increased size for timestamp-pid ID (e.g., 16897...-12345)
    char date_str[11]; // YYYY-MM-DD
    char time_str[9];  // HH:MM:SS
    char customer_name[50];
    int medicine_code;
    char medicine_name[40];
    int quantity;
    float price_per_item;
    float total_cost; // Cost for this specific line item
};

// --- BST Node Structure (Renamed from TreeNode) ---
typedef struct node {
    struct medicine data; // Store the actual data
    struct node *left;
    struct node *right;
} node; // Use 'node' as the type name

// --- Hash Table Node Structure (for Separate Chaining) ---
typedef struct HashNode {
    struct medicine data; // Store the actual data
    struct HashNode *next;
} HashNode;

// --- Structure for handling multiple billing items (Unchanged) ---
struct bill_item_request {
    int code;
    int quantity_requested;
    // Fields populated during validation
    int found_in_stock;        // 0 = no, 1 = yes
    int sufficient_stock;      // 0 = no, 1 = yes
    int stock_validation_done; // 0 = no, 1 = yes (to avoid re-validating)
    float price_per_item;
    char name[40];
    int original_stock_qty;
    int new_stock_qty; // Calculated new quantity IF successful
    char error_msg[100]; // To store specific error for this item
    struct medicine* stock_data_ptr; // Pointer to the medicine data in hash table (for easy update)
};


// --- Global Data Structures ---
// Initialized in main, freed in main
HashNode **globalHashTable = NULL;
int globalHashTableSize = HASH_TABLE_SIZE;
node *globalBstRoot = NULL;


// --- Function Prototypes ---

// Helpers
void urlDecode(char *dst, const char *src);
char* get_param(const char *data, const char *param_name);
const char *stristr(const char *haystack, const char *needle);
int parse_multi_value_param(const char *data, const char *param_name, char **values, int max_values);
char* get_csv_field(char **line_ptr, int *is_quoted); // For robust CSV parsing

// Hashing Functions
unsigned int hashFunction(int key, int tableSize);
HashNode** createHashTable(int size);
int insertIntoHashTable(HashNode **table, int tableSize, struct medicine med); // Returns 1 on success, 0 on duplicate/error
struct medicine* searchHashTableByCode(HashNode **table, int tableSize, int code); // Returns pointer to medicine data or NULL
void freeHashTable(HashNode **table, int tableSize);
int updateHashTableQuantity(HashNode **table, int tableSize, int code, int new_quantity);

// BST Functions (using 'struct node')
node* createBstNode(struct medicine med);
node* insertBstNode(node* root, struct medicine med);
node* searchBSTByCode(node* root, int code);
void searchBSTByNameSubstring(node* root, const char* nameQuery, int* matchCount); // Prints matches
void freeTree(node* root);
int updateBstQuantity(node* root, int code, int new_quantity);
void printBstInOrder(node *root); // Modified for Rupee symbol
void checkExpiryRecursive(node *root, time_t today_start_t, time_t warning_start_t, int *relevant_items_found, int warning_days);

// Data Loading
int loadStockData(const char* filename, HashNode ***hashTablePtr, int *hashTableSizePtr, node **bstRootPtr); // Returns 1 on success, 0 on failure

// Core Logic Functions
void processAddStock(char *post_data);
void viewStock(); // Uses BST traversal (modified for Rupee symbol)
void processUpdateStock(char *request_data); // Uses Hash find, rewrites file, updates memory
int saveSaleRecord(const struct sale_record *sale); // Modified for Invoice ID
void processBillingMultiple(char *request_data); // Modified for Invoice ID
void checkExpiry(); // Uses BST traversal
void generateReport(); // Modified for Invoice ID and Rupee Symbol in totals
void searchMedicine(char *request_data); // Modified for Rupee symbol

// --- Helper Function Implementations ---

// urlDecode, get_param, parse_multi_value_param, stristr remain unchanged...
void urlDecode(char *dst, const char *src) {
    char a, b; if (!dst || !src) { fprintf(stderr, "urlDecode: NULL pointer.\n"); fflush(stderr); return; }
    size_t src_len = strlen(src); char *dst_start = dst;
    while (*src && (size_t)(dst - dst_start) < src_len + 1 ) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
             if (a >= 'a') a -= 'a'-'A'; if (a >= 'A') a -= ('A' - 10); else a -= '0';
             if (b >= 'a') b -= 'a'-'A'; if (b >= 'A') b -= ('A' - 10); else b -= '0';
            *dst++ = 16*a + b; src+=3;
        } else if (*src == '+') { *dst++ = ' '; src++; }
        else { *dst++ = *src++; }
    } *dst = '\0';
}

char* get_param(const char *data, const char *param_name) {
    if (data == NULL || param_name == NULL || *data == '\0') { return NULL; }
    const char *pair_start = data; char *found_value_encoded = NULL; char *found_value_decoded = NULL;
    size_t param_len = strlen(param_name);
    while (pair_start && *pair_start) {
        const char *pair_end = strchr(pair_start, '&'); size_t current_pair_len = pair_end ? (pair_end - pair_start) : strlen(pair_start);
        const char *eq_pos = NULL; size_t i; for (i = 0; i < current_pair_len; ++i) { if (pair_start[i] == '=') { eq_pos = pair_start + i; break; } }
        if (eq_pos && (size_t)(eq_pos - pair_start) == param_len && strncmp(pair_start, param_name, param_len) == 0) {
            const char *value_start = eq_pos + 1; size_t value_len = (pair_start + current_pair_len) - value_start;
            found_value_encoded = (char *)malloc(value_len + 1); if (!found_value_encoded) { return NULL; }
            strncpy(found_value_encoded, value_start, value_len); found_value_encoded[value_len] = '\0';
            found_value_decoded = (char *)malloc(value_len + 1); if (!found_value_decoded) { free(found_value_encoded); return NULL; }
            urlDecode(found_value_decoded, found_value_encoded); free(found_value_encoded); return found_value_decoded;
        } if (pair_end) { pair_start = pair_end + 1; } else { break; }
    } return NULL;
}

int parse_multi_value_param(const char *data, const char *param_name, char **values, int max_values) {
    if (data == NULL || param_name == NULL || values == NULL || max_values <= 0) { return 0; }
    const char *pair_start = data; size_t param_len = strlen(param_name); int count = 0;
    while (pair_start && *pair_start && count < max_values) {
        const char *pair_end = strchr(pair_start, '&'); size_t current_pair_len = pair_end ? (pair_end - pair_start) : strlen(pair_start);
        const char *eq_pos = NULL; size_t i; for (i = 0; i < current_pair_len; ++i) { if (pair_start[i] == '=') { eq_pos = pair_start + i; break; } }
        if (eq_pos && (size_t)(eq_pos - pair_start) == param_len && strncmp(pair_start, param_name, param_len) == 0) {
            const char *value_start = eq_pos + 1; size_t value_len = (pair_start + current_pair_len) - value_start;
            char *encoded_val = (char *)malloc(value_len + 1); char *decoded_val = (char *)malloc(value_len + 1);
            if (!encoded_val || !decoded_val) { fprintf(stderr, "parse_multi_value_param: Mem alloc failed.\n"); if (encoded_val) free(encoded_val); if (decoded_val) free(decoded_val); for (int k = 0; k < count; k++) { free(values[k]); values[k] = NULL; } return -1; }
            strncpy(encoded_val, value_start, value_len); encoded_val[value_len] = '\0';
            urlDecode(decoded_val, encoded_val); free(encoded_val); values[count++] = decoded_val;
        } if (pair_end) { pair_start = pair_end + 1; } else { break; }
    } return count;
}

const char *stristr(const char *haystack, const char *needle) {
    if (!needle || !*needle) { return haystack; } if (!haystack) { return NULL; }
    while (*haystack) { const char *h = haystack; const char *n = needle; while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) { h++; n++; } if (!*n) { return haystack; } haystack++; } return NULL;
}

// Robust CSV field extractor (Unchanged)
char* get_csv_field(char **line_ptr, int *is_quoted) {
    if (line_ptr == NULL || *line_ptr == NULL || **line_ptr == '\0') return NULL;

    char *start;
    char *ptr = *line_ptr;
    char *field_end = NULL;
    *is_quoted = 0;

    // Trim leading whitespace (optional, but good practice)
    while (isspace((unsigned char)*ptr)) ptr++;

    // Check if field starts with a quote
    if (*ptr == '"') {
        *is_quoted = 1;
        ptr++; // Move past the opening quote
        start = ptr; // Actual data starts here
        while (*ptr) {
            if (*ptr == '"') {
                // Check for escaped quote ("")
                if (*(ptr + 1) == '"') {
                    // Copy the content before the escape sequence
                    memmove(ptr, ptr + 1, strlen(ptr)); // Shift rest of string left by 1
                    // ptr remains at the current position (which now holds the second quote)
                     ptr++; // Move past the single quote that was kept
                } else {
                    // This is the closing quote
                    field_end = ptr; // Mark the position of the closing quote
                    *field_end = '\0'; // Null-terminate the field content
                    ptr++; // Move past the (now null) closing quote
                    // Find the next comma or end of line after the closing quote
                    while (*ptr && *ptr != ',') {
                         if (!isspace((unsigned char)*ptr)) {
                              // Allow non-space characters after quotes, as the invoice ID might contain dashes
                              // fprintf(stderr, "get_csv_field: Warning - Non-space char after closing quote: '%c'\n", *ptr);
                         }
                         ptr++;
                    }
                    if (*ptr == ',') ptr++; // Consume the comma
                    *line_ptr = ptr; // Update the main line pointer
                    return start; // Return the field data
                }
            } else {
                ptr++;
            }
        }
         // If loop finished without finding closing quote, it's malformed or end of line
         *field_end = '\0'; // Null terminate at current position
         *line_ptr = ptr; // Update the main line pointer
         return start; // Return whatever was parsed

    } else {
        // Unquoted field
        start = ptr;
        while (*ptr && *ptr != ',') {
            // Special case for invoice ID which might contain '-'
            if (*ptr == '"') {
                // An unquoted field should ideally not contain quotes.
                // Handle as error or just continue parsing? Let's continue.
                fprintf(stderr, "get_csv_field: Warning - Quote found in unquoted field near '%s'\n", start);
            }
            ptr++;
        }
        field_end = ptr;
        if (*ptr == ',') {
            *ptr = '\0'; // Null-terminate the field
            ptr++; // Move past the comma
        } else {
             *ptr = '\0'; // Null-terminate at end of line
        }
        *line_ptr = ptr; // Update the main line pointer

        // Trim trailing whitespace from unquoted field
        char *trimmed_end = field_end - 1;
        while(trimmed_end >= start && isspace((unsigned char)*trimmed_end)) {
            *trimmed_end = '\0';
            trimmed_end--;
        }
        return start; // Return the unquoted field
    }
}


// --- Hashing Function Implementations ---

// hashFunction, createHashTable, insertIntoHashTable, searchHashTableByCode, freeHashTable, updateHashTableQuantity remain unchanged...
unsigned int hashFunction(int key, int tableSize) {
    unsigned int hash = (key < 0) ? (unsigned int)(-key) : (unsigned int)key;
    return hash % tableSize;
}

HashNode** createHashTable(int size) {
    if (size <= 0) { fprintf(stderr, "Error: Invalid hash table size (%d).\n", size); return NULL; }
    HashNode **table = (HashNode **)calloc(size, sizeof(HashNode *));
    if (table == NULL) { fprintf(stderr, "Error: Mem alloc failed for hash table (size %d).\n", size); }
    else { fprintf(stderr, "Hash table created (size %d).\n", size); }
    return table;
}

int insertIntoHashTable(HashNode **table, int tableSize, struct medicine med) {
    if (table == NULL) return -1;
    unsigned int index = hashFunction(med.mcode, tableSize);
    HashNode *current = table[index];
    while (current != NULL) { if (current->data.mcode == med.mcode) { fprintf(stderr, "Warn: Duplicate code %d in hash insert.\n", med.mcode); return 0; } current = current->next; }
    HashNode *newNode = (HashNode *)malloc(sizeof(HashNode));
    if (newNode == NULL) { fprintf(stderr, "Error: Mem alloc failed hash node (code %d).\n", med.mcode); return -1; }
    newNode->data = med; newNode->next = table[index]; table[index] = newNode; return 1;
}

struct medicine* searchHashTableByCode(HashNode **table, int tableSize, int code) {
    if (table == NULL) return NULL;
    unsigned int index = hashFunction(code, tableSize);
    HashNode *current = table[index];
    while (current != NULL) { if (current->data.mcode == code) { return &(current->data); } current = current->next; }
    return NULL;
}

void freeHashTable(HashNode **table, int tableSize) {
    if (table == NULL) return;
    fprintf(stderr, "Freeing hash table...\n");
    for (int i = 0; i < tableSize; i++) { HashNode *current = table[i]; while (current != NULL) { HashNode *temp = current; current = current->next; free(temp); } table[i] = NULL; }
    free(table); fprintf(stderr, "Hash table freed.\n");
}

int updateHashTableQuantity(HashNode **table, int tableSize, int code, int new_quantity) {
    if (table == NULL) return -1;
    struct medicine* med_ptr = searchHashTableByCode(table, tableSize, code);
    if (med_ptr != NULL) { med_ptr->quantity = new_quantity; fprintf(stderr, "Hash qty updated code %d -> %d.\n", code, new_quantity); return 1; }
    fprintf(stderr, "Warn: Code %d not found in hash for qty update.\n", code); return 0;
}


// --- BST Function Implementations ---

// createBstNode, insertBstNode, searchBSTByCode remain unchanged...
node* createBstNode(struct medicine med) {
    node* newNode = (node*)malloc(sizeof(node)); if (newNode == NULL) { fprintf(stderr, "Error: Mem alloc failed BST node.\n"); return NULL; }
    newNode->data = med; newNode->left = newNode->right = NULL; return newNode;
}

node* insertBstNode(node* root, struct medicine med) {
    if (root == NULL) { return createBstNode(med); }
    if (med.mcode < root->data.mcode) { root->left = insertBstNode(root->left, med); }
    else if (med.mcode > root->data.mcode) { root->right = insertBstNode(root->right, med); }
    else { fprintf(stderr, "Warn: Duplicate code %d in BST insert.\n", med.mcode); }
    return root;
}

node* searchBSTByCode(node* root, int code) {
    if (root == NULL || root->data.mcode == code) { return root; }
    if (code < root->data.mcode ) { return searchBSTByCode(root->left, code); }
    return searchBSTByCode(root->right, code);
}

// Modified searchBSTByNameSubstring to add Rupee symbol
void searchBSTByNameSubstring(node* root, const char* nameQuery, int* matchCount) {
    if (root == NULL || nameQuery == NULL || matchCount == NULL) { return; }
    searchBSTByNameSubstring(root->left, nameQuery, matchCount);
    if (stristr(root->data.name, nameQuery) != NULL) {
        (*matchCount)++; struct medicine m = root->data;
        // Added Rupee Symbol below
        printf("<tr><td>%d</td><td>%s</td><td>%s</td><td>%lld</td><td>₹%.2f</td><td style='text-align:center;'>%d</td><td>%04d-%02d-%02d</td></tr>\n",
               m.mcode, m.name, m.s_name, m.s_contact, m.price, m.quantity, m.year, m.month, m.day); fflush(stdout); }
    searchBSTByNameSubstring(root->right, nameQuery, matchCount);
}

// freeTree, updateBstQuantity remain unchanged...
void freeTree(node* root) {
    if (root == NULL) { return; } freeTree(root->left); freeTree(root->right); free(root);
}

int updateBstQuantity(node* root, int code, int new_quantity) {
    node* targetNode = searchBSTByCode(root, code);
    if (targetNode != NULL) { targetNode->data.quantity = new_quantity; fprintf(stderr, "BST qty updated code %d -> %d.\n", code, new_quantity); return 1; }
    fprintf(stderr, "Warn: Code %d not found in BST for qty update.\n", code); return 0;
}

// Modified printBstInOrder to add Rupee symbol
void printBstInOrder(node *root) {
    if (root != NULL) {
        printBstInOrder(root->left); struct medicine m = root->data;
        // Added Rupee Symbol below
        printf("<tr><td>%d</td><td>%s</td><td>%s</td><td>%lld</td><td>₹%.2f</td><td style='text-align:center;'>%d</td><td>%04d-%02d-%02d</td></tr>\n",
               m.mcode, m.name, m.s_name, m.s_contact, m.price, m.quantity, m.year, m.month, m.day); fflush(stdout);
        printBstInOrder(root->right);
    }
}

// checkExpiryRecursive remains unchanged...
void checkExpiryRecursive(node *root, time_t today_start_t, time_t warning_start_t, int *relevant_items_found, int warning_days) {
    if (root == NULL) { return; } checkExpiryRecursive(root->left, today_start_t, warning_start_t, relevant_items_found, warning_days);
    struct medicine m = root->data; struct tm expiry_tm = {0}; expiry_tm.tm_year = m.year - 1900; expiry_tm.tm_mon = m.month - 1; expiry_tm.tm_mday = m.day; expiry_tm.tm_isdst = -1; time_t expiry_t = mktime(&expiry_tm);
    char *status_class = NULL, *status_text = NULL, *row_class = NULL;
    if (expiry_t == (time_t)-1) { fprintf(stderr, "Warn: Cannot convert expiry %04d-%02d-%02d code %d.\n", m.year, m.month, m.day, m.mcode); }
    else if (expiry_t < today_start_t) { status_class = "status-expired"; status_text = "Expired"; row_class = "status-expired"; (*relevant_items_found)++; }
    else if (expiry_t < warning_start_t) { status_class = "status-warning"; status_text = "Expiring Soon"; row_class = "status-warning"; (*relevant_items_found)++; }
    if (status_class != NULL) { printf("<tr class='%s'><td>%s</td><td>%d</td><td>%04d-%02d-%02d</td><td style='text-align: center;'><span class='status-cell %s'>%s</span></td></tr>\n", row_class, m.name, m.mcode, m.year, m.month, m.day, status_class, status_text); fflush(stdout); }
    checkExpiryRecursive(root->right, today_start_t, warning_start_t, relevant_items_found, warning_days);
}


// --- Data Loading Implementation ---

// loadStockData remains unchanged...
int loadStockData(const char* filename, HashNode ***hashTablePtr, int *hashTableSizePtr, node **bstRootPtr) {
    fprintf(stderr, "loadStockData: Loading from %s\n", filename); FILE *fp = fopen(filename, "r");
    if (fp == NULL) { if (errno == ENOENT) { fprintf(stderr, "loadStockData: File %s not found. OK.\n", filename); return 1; } else { fprintf(stderr, "FATAL: Error opening %s: %s\n", filename, strerror(errno)); return 0; } }
    if (*hashTablePtr == NULL || *bstRootPtr != NULL) { fprintf(stderr, "loadStockData: Error - Structures not pre-initialized.\n"); fclose(fp); return 0; }
    struct medicine m; char line[256]; int line_num = 0, items_loaded_hash = 0, items_loaded_bst = 0, hash_insert_result;
    while (fgets(line, sizeof(line), fp)) {
        line_num++; line[strcspn(line, "\r\n")] = 0; if (strspn(line, " \t") == strlen(line)) continue;
        memset(&m, 0, sizeof(m)); int items_parsed = sscanf(line, "%39[^,],%d,%49[^,],%lld,%f,%d,%d,%d,%d", m.name, &m.mcode, m.s_name, &m.s_contact, &m.price, &m.quantity, &m.year, &m.month, &m.day);
        if (items_parsed == 9) { hash_insert_result = insertIntoHashTable(*hashTablePtr, *hashTableSizePtr, m);
            if (hash_insert_result == 1) { items_loaded_hash++; *bstRootPtr = insertBstNode(*bstRootPtr, m); if (*bstRootPtr == NULL && items_loaded_hash == 1) { fprintf(stderr, "FATAL: BST insert failed.\n"); fclose(fp); return 0; } if (*bstRootPtr != NULL) { items_loaded_bst++;} }
            else if (hash_insert_result == -1) { fprintf(stderr, "FATAL: Hash insert failed.\n"); fclose(fp); return 0; } }
        else { fprintf(stderr, "loadStockData: Malformed line %d in %s.\n", line_num, filename); } }
    if (ferror(fp)) { fprintf(stderr, "loadStockData: Error reading %s: %s\n", filename, strerror(errno)); } fclose(fp);
    fprintf(stderr, "loadStockData: Loaded %d hash, %d BST.\n", items_loaded_hash, items_loaded_bst); return 1;
}


// --- Core Logic Functions ---

// processAddStock remains unchanged...
void processAddStock(char *post_data) {
    fprintf(stderr, "processAddStock: Started.\n"); struct medicine m; memset(&m, 0, sizeof(m)); char *temp = NULL; int y=0, mo=0, d=0, parse_error = 0;
    temp = get_param(post_data, "medicineName"); if (temp) { strncpy(m.name, temp, 39); m.name[39] = '\0'; free(temp); } else { parse_error=1; fprintf(stderr,"Missing Name\n");}
    temp = get_param(post_data, "medicineCode"); if (temp) { m.mcode = atoi(temp); free(temp); } else { parse_error=1; fprintf(stderr,"Missing Code\n");}
    temp = get_param(post_data, "suppliername"); if (temp) { strncpy(m.s_name, temp, 49); m.s_name[49] = '\0'; free(temp); } else { parse_error=1; fprintf(stderr,"Missing S.Name\n");}
    temp = get_param(post_data, "suppliercontact"); if (temp) { m.s_contact = atoll(temp); free(temp); } else { parse_error=1; fprintf(stderr,"Missing S.Contact\n");}
    temp = get_param(post_data, "price"); if (temp) { m.price = atof(temp); free(temp); } else { parse_error=1; fprintf(stderr,"Missing Price\n");}
    temp = get_param(post_data, "quantity"); if (temp) { m.quantity = atoi(temp); free(temp); } else { parse_error=1; fprintf(stderr,"Missing Qty\n");}
    temp = get_param(post_data, "expiry"); if (temp) { if (sscanf(temp, "%d-%d-%d", &y, &mo, &d) == 3) { m.year = y; m.month = mo; m.day = d; } else { parse_error=1; printf("<p class='error'>Invalid Expiry '%s'.</p>", temp); fflush(stdout); } free(temp); } else { parse_error=1; fprintf(stderr,"Missing Expiry\n"); }
    int validation_failed = (parse_error || strlen(m.name) == 0 || m.mcode <= 0 || strlen(m.s_name) == 0 || m.s_contact <= 0 || m.quantity <= 0 || m.price < 0 || m.year < 1970 || m.month < 1 || m.month > 12 || m.day < 1 || m.day > 31);
    if (validation_failed) { fprintf(stderr, "Add Validation Failed.\n"); printf("<h2>Error Adding</h2><p class='error'>Invalid/missing data.</p><p><a href='../add_stock.html' class='btn btn-secondary'>Back</a></p>"); fflush(stdout); return; }
    if (searchHashTableByCode(globalHashTable, globalHashTableSize, m.mcode) != NULL) { fprintf(stderr, "Add Error: Code %d exists.\n", m.mcode); printf("<h2>Error Adding</h2><p class='error'>Code %d already exists.</p><p><a href='../add_stock.html' class='btn btn-secondary'>Back</a></p>", m.mcode); fflush(stdout); return; }
    FILE *fp = fopen(STOCK_FILE, "a"); if (fp == NULL) { fprintf(stderr, "FATAL: Error opening %s: %s\n", STOCK_FILE, strerror(errno)); printf("<h2>Internal Error</h2><p class='error'>Cannot open file.</p>"); fflush(stdout); return; }
    int write_result = fprintf(fp, "%s,%d,%s,%lld,%.2f,%d,%d,%d,%d\n", m.name, m.mcode, m.s_name, m.s_contact, m.price, m.quantity, m.year, m.month, m.day); fclose(fp);
    if (write_result < 0) { fprintf(stderr, "Error writing %s: %s\n", STOCK_FILE, strerror(errno)); printf("<h2>Error Adding</h2><p class='error'>Failed write.</p><p><a href='../add_stock.html' class='btn btn-secondary'>Back</a></p>"); fflush(stdout); }
    else { fprintf(stderr, "Written code %d. Adding mem.\n", m.mcode); int hash_add = insertIntoHashTable(globalHashTable, globalHashTableSize, m);
        if (hash_add == 1) { globalBstRoot = insertBstNode(globalBstRoot, m); fprintf(stderr, "Added code %d hash/BST.\n", m.mcode); printf("<div class='success'><h2>Stock Added</h2><p>%s (%d)</p><p>Qty: %d</p><p>%04d-%02d-%02d</p><p><a href='../add_stock.html' class='btn btn-primary'>Add Another</a>|<a href='medical.exe' class='btn btn-secondary'>View</a></p></div>", m.name, m.mcode, m.quantity, m.year, m.month, m.day); fflush(stdout); }
        else if (hash_add == 0){ fprintf(stderr, "Warn: Code %d already in hash?\n", m.mcode); printf("<h2>Internal Warning</h2><p class='warning'>File saved, error live view.</p>"); }
        else { fprintf(stderr, "FATAL: Mem error add code %d.\n", m.mcode); printf("<h2>Internal Error</h2><p class='error'>File saved, mem error live view.</p>"); } }
    fprintf(stderr, "processAddStock: Finished.\n"); fflush(stderr);
}

// Modified viewStock to use the updated printBstInOrder (which includes Rupee symbol)
void viewStock() {
    fprintf(stderr, "viewStock: Called.\n"); printf("<div style='overflow-x:auto;'><table class='stock-table'><thead><tr><th>Code</th><th>Name</th><th>Supplier</th><th>Contact</th><th>Price</th><th>Quantity</th><th>Expiry Date</th></tr></thead><tbody>"); fflush(stdout);
    if (globalBstRoot == NULL) { fprintf(stderr, "viewStock: BST empty.\n"); printf("<tr><td colspan='7' style='text-align:center; font-style:italic;'>No stock.</td></tr>"); } else { printBstInOrder(globalBstRoot); } // This now prints with ₹
    printf("</tbody></table></div>"); fprintf(stderr, "viewStock: Finished.\n"); fflush(stderr);
}

// processUpdateStock remains unchanged...
void processUpdateStock(char *request_data) {
    fprintf(stderr, "processUpdateStock: Started.\n"); char *code_str = get_param(request_data, "medicineCode"), *qty_add_str = get_param(request_data, "newQuantity"), *name_str = get_param(request_data, "medicineName");
    char tname[40] = "N/A"; int code = 0, qty_change = 0, final_qty = 0, validation_error = 0;
    if (!code_str || strlen(code_str) == 0) { printf("<p class='error'>Code needed.</p>"); validation_error = 1; } else { char *e; errno=0; long c=strtol(code_str,&e,10); if(errno!=0||*e!='\0'||c<=0||c>INT_MAX){ printf("<p class='error'>Invalid Code.</p>");validation_error=1;} else code=(int)c; }
    if (!qty_add_str || strlen(qty_add_str)==0) { printf("<p class='error'>Qty needed.</p>"); validation_error=1; } else { char *e; errno=0; long q=strtol(qty_add_str,&e,10); if(errno!=0||*e!='\0'||q>INT_MAX||q<INT_MIN){ printf("<p class='error'>Invalid Qty.</p>");validation_error=1;} else qty_change=(int)q; }
    if (code_str) free(code_str); if (qty_add_str) free(qty_add_str); if (validation_error) { printf("<p><a href='../update_stock.html' class='btn'>Back</a>|<a href='medical.exe' class='btn'>View</a></p>"); if (name_str) free(name_str); fprintf(stderr, "Update validation failed.\n"); fflush(stderr); return; }
    fprintf(stderr, "Update Req: Code=%d, Change=%d\n", code, qty_change); struct medicine* med_ptr = searchHashTableByCode(globalHashTable, globalHashTableSize, code);
    if (med_ptr == NULL) { fprintf(stderr, "Update Error: Code %d not found.\n", code); printf("<div class='error'>Code %d not found.</div>", code); printf("<p><a href='../update_stock.html' class='btn'>Back</a>|<a href='medical.exe' class='btn'>View</a></p>"); if (name_str) free(name_str); return; }
    strncpy(tname, med_ptr->name, 39); tname[39]='\0'; final_qty = med_ptr->quantity + qty_change; if (final_qty < 0) { fprintf(stderr, "Warn: Update %d -> neg stock. Set 0.\n", code); printf("<p class='warning'>Warn: Update %s (%d) -> neg stock. Set 0.</p>", tname, code); final_qty = 0; }
    FILE *in = fopen(STOCK_FILE, "r"), *out = fopen(TEMP_STOCK_FILE_UPDATE, "w"); int file_error = 0, found_in_file = 0; if (!in || !out) { fprintf(stderr, "FATAL: Cannot open files update! %s\n", strerror(errno)); printf("<p class='error'>Internal Error: files.</p>"); if(in)fclose(in); if(out)fclose(out); remove(TEMP_STOCK_FILE_UPDATE); if (name_str) free(name_str); return; }
    struct medicine m_line; char line[512], orig_line[512]; int ln = 0;
    while (fgets(line, sizeof(line), in)) { ln++; strcpy(orig_line, line); line[strcspn(line, "\r\n")] = 0; if (strspn(line, " \t")==strlen(line)) { if (fputs(orig_line, out)==EOF) { file_error=1; break; } continue; }
        int line_code = 0; sscanf(line, "%*[^,],%d", &line_code);
        if (line_code == code) { found_in_file=1; int items = sscanf(line, "%39[^,],%d,%49[^,],%lld,%f,%d,%d,%d,%d", m_line.name,&m_line.mcode,m_line.s_name,&m_line.s_contact,&m_line.price,&m_line.quantity,&m_line.year,&m_line.month,&m_line.day);
             if (items!=9) { fprintf(stderr, "Parse err ln %d update.\n", ln); if (fputs(orig_line, out)==EOF) { file_error=1; break; } continue; }
            if (fprintf(out, "%s,%d,%s,%lld,%.2f,%d,%d,%d,%d\n", m_line.name,m_line.mcode,m_line.s_name,m_line.s_contact,m_line.price,final_qty,m_line.year,m_line.month,m_line.day) < 0) { file_error=1; break; }
        } else { if (fputs(orig_line, out)==EOF) { file_error=1; break; } } if (ferror(out)) { file_error=1; break; } }
    if (ferror(in)) { file_error=1; } fclose(in); if (fclose(out)!=0) { file_error=1; } if (!found_in_file && !file_error) { fprintf(stderr, "Error: Code %d mem not file!\n", code); printf("<p class='error'>Internal Inconsistency (Code: %d).</p>", code); file_error=1; }
    if (file_error) { fprintf(stderr, "Update fail: file IO err. Clean %s.\n", TEMP_STOCK_FILE_UPDATE); remove(TEMP_STOCK_FILE_UPDATE); printf("<div class='error'>Internal file error. Stock not modified.</div>"); }
    else { fprintf(stderr, "File rewrite OK %d. Replace.\n", code);
        if (remove(STOCK_FILE)!=0) { fprintf(stderr, "CRIT: Fail remove %s! %s\n", STOCK_FILE, strerror(errno)); remove(TEMP_STOCK_FILE_UPDATE); printf("<div class='error'>CRIT ERR: Cannot remove orig file. Not saved.</div>"); file_error=1; }
        else if (rename(TEMP_STOCK_FILE_UPDATE, STOCK_FILE)!=0) { fprintf(stderr, "CRIT: Fail rename %s->%s! %s\n", TEMP_STOCK_FILE_UPDATE, STOCK_FILE, strerror(errno)); printf("<div class='error'>CRIT ERR: Cannot rename temp. Data in '%s'.</div>", TEMP_STOCK_FILE_UPDATE); file_error=1; }
        else { fprintf(stderr, "File updated %d. Update mem.\n", code); int h_upd = updateHashTableQuantity(globalHashTable, globalHashTableSize, code, final_qty); int b_upd = updateBstQuantity(globalBstRoot, code, final_qty);
            if (h_upd && b_upd) { fprintf(stderr, "Mem updated %d.\n", code); printf("<div class='success'><h2>Stock Updated</h2><p>%s (%d)</p><p>Change: %d</p><p>New Qty: %d</p><p><a href='../update_stock.html' class='btn'>Update Another</a>|<a href='medical.exe' class='btn'>View</a></p></div>", tname, code, qty_change, final_qty); }
            else { fprintf(stderr, "Err: Mem update fail %d (H:%d, B:%d)\n", code, h_upd, b_upd); printf("<div class='warning'><h2>Update Partial</h2><p>File updated, live view error.</p><p>%s (%d)</p><p>New Qty: %d</p><p><a href='../update_stock.html' class='btn'>Update Another</a>|<a href='medical.exe' class='btn'>View</a></p></div>", tname, code, final_qty); } } }
    if (file_error) { printf("<p><a href='../update_stock.html' class='btn'>Back</a>|<a href='medical.exe' class='btn'>View</a></p>"); }
    fflush(stdout); if (name_str) free(name_str); fprintf(stderr, "processUpdateStock: Finished.\n"); fflush(stderr);
}

// Modified saveSaleRecord to include Invoice ID
int saveSaleRecord(const struct sale_record *sale) {
    FILE *fp = fopen(SALES_FILE, "a"); if (fp == NULL) { fprintf(stderr, "Err opening sales %s: %s\n", SALES_FILE, strerror(errno)); return 0; }
    fseek(fp, 0, SEEK_END); long size = ftell(fp);
    if (size == 0) {
        // Write header with InvoiceID
        fprintf(fp, "InvoiceID,Date,Time,CustomerName,MedicineCode,MedicineName,Quantity,PricePerItem,TotalCost\n");
    } else {
        // Ensure newline before appending data
        fseek(fp, -1, SEEK_END);
        if (fgetc(fp) != '\n') {
            fprintf(fp, "\n");
        }
        fseek(fp, 0, SEEK_END); // Go back to end for appending
    }
    // Write sale data including InvoiceID (ensure it's quoted if it contains commas, though unlikely with timestamp-pid)
    // Using "%s" for invoice ID assumes it doesn't contain quotes or commas. If it might, it should be quoted properly.
    int r = fprintf(fp, "\"%s\",%s,%s,\"%s\",%d,\"%s\",%d,%.2f,%.2f\n",
                    sale->invoice_id, sale->date_str, sale->time_str, sale->customer_name,
                    sale->medicine_code, sale->medicine_name, sale->quantity,
                    sale->price_per_item, sale->total_cost);
    fclose(fp);
    if (r < 0) { fprintf(stderr, "Err writing sales %s: %s\n", SALES_FILE, strerror(errno)); return 0; }
    fprintf(stderr, "Sale saved: Inv# %s, Cust %s, Code %d, Qty %d\n", sale->invoice_id, sale->customer_name, sale->medicine_code, sale->quantity); return 1;
}


// Modified processBillingMultiple to generate, display, and save Invoice ID
void processBillingMultiple(char *request_data) {
    fprintf(stderr, "processBillingMultiple: Started.\n"); char *cust_raw = NULL, *code_s[MAX_BILL_ITEMS] = {NULL}, *qty_s[MAX_BILL_ITEMS] = {NULL}; struct bill_item_request req_items[MAX_BILL_ITEMS]; char cust_name[50] = "";
    int n_codes = 0, n_qtys = 0, n_items = 0, err = 0, valid = 1, stock_upd_ok = 0, sales_saved = 0, saved_count = 0 ; double grand_total = 0.0;
    // Invoice ID generation variable
    char generated_invoice_id[30] = "";

    cust_raw = get_param(request_data, "customerName"); if (!cust_raw || strlen(cust_raw) == 0) { printf("<p class='error'>Customer Name needed.</p>"); err = 1; } else { strncpy(cust_name, cust_raw, 49); cust_name[49] = '\0'; for (int i = 0; cust_name[i]; i++) { if (strchr("<>\"", cust_name[i])) { printf("<p class='error'>Invalid chars in Name.</p>"); err = 1; break; } } free(cust_raw); }
    n_codes = parse_multi_value_param(request_data, "medicineCode%5B%5D", code_s, MAX_BILL_ITEMS); n_qtys = parse_multi_value_param(request_data, "quantity%5B%5D", qty_s, MAX_BILL_ITEMS);
    fprintf(stderr, "Parsed %d codes, %d qtys.\n", n_codes, n_qtys);
    if (n_codes <= 0 || n_qtys <= 0) { printf("<p class='error'>No items.</p>"); err = 1; } else if (n_codes != n_qtys) { printf("<p class='error'>Code/Qty mismatch.</p>"); err = 1; } else if (n_codes == -1 || n_qtys == -1) { printf("<p class='error'>Internal Error: Mem fail parse.</p>"); err = 1; }
    else { n_items = n_codes;
        for (int i = 0; i < n_items; i++) { memset(&req_items[i], 0, sizeof(req_items[0])); strcpy(req_items[i].error_msg, ""); long c_val = 0, q_val = 0; char *e_c, *e_q; errno = 0;
            if (!code_s[i] || strlen(code_s[i])==0) { snprintf(req_items[i].error_msg, 100, "Item %d: No Code.", i+1); valid=0; } else { c_val=strtol(code_s[i],&e_c,10); if(errno!=0||*e_c!='\0'||c_val<=0||c_val>INT_MAX){ snprintf(req_items[i].error_msg, 100, "Item %d: Bad Code '%s'.", i+1, code_s[i]); valid=0;} else req_items[i].code=(int)c_val; }
            if (!qty_s[i] || strlen(qty_s[i])==0) { snprintf(req_items[i].error_msg, 100, "Item %d (C%d): No Qty.", i+1, req_items[i].code); valid=0; } else { q_val=strtol(qty_s[i],&e_q,10); if(errno!=0||*e_q!='\0'||q_val<=0||q_val>INT_MAX){ snprintf(req_items[i].error_msg, 100, "Item %d (C%d): Bad Qty '%s'.", i+1, req_items[i].code, qty_s[i]); valid=0;} else req_items[i].quantity_requested=(int)q_val; } } }
    for (int i = 0; i < n_codes; i++) { if (code_s[i]) free(code_s[i]); } for (int i = 0; i < n_qtys; i++) { if (qty_s[i]) free(qty_s[i]); }
    if (err || !valid) { printf("<h3>Billing Errors</h3>"); for (int i = 0; i < n_items; i++) { if (strlen(req_items[i].error_msg) > 0) { printf("<p class='error'>%s</p>", req_items[i].error_msg); } } printf("<p><a href='../billing.html' class='btn'>Back</a></p>"); fflush(stdout); fprintf(stderr, "Billing abort: input validation.\n"); return; }
    fprintf(stderr, "Input OK %d items for '%s'. Validate stock hash.\n", n_items, cust_name);
    valid = 1; for (int i = 0; i < n_items; i++) { struct medicine* med = searchHashTableByCode(globalHashTable, globalHashTableSize, req_items[i].code);
        if (med == NULL) { req_items[i].found_in_stock = 0; snprintf(req_items[i].error_msg, 100, "Code %d not found.", req_items[i].code); fprintf(stderr, " [FAIL] Code %d: Not found hash.\n", req_items[i].code); valid = 0; }
        else { req_items[i].found_in_stock = 1; req_items[i].stock_data_ptr = med; strncpy(req_items[i].name, med->name, 39); req_items[i].name[39] = '\0'; req_items[i].price_per_item = med->price; req_items[i].original_stock_qty = med->quantity;
            if (med->quantity >= req_items[i].quantity_requested) { req_items[i].sufficient_stock = 1; req_items[i].new_stock_qty = med->quantity - req_items[i].quantity_requested; fprintf(stderr, " [OK] C%d (%s): Stock %d >= Req %d. New %d\n", req_items[i].code, req_items[i].name, med->quantity, req_items[i].quantity_requested, req_items[i].new_stock_qty); }
            else { req_items[i].sufficient_stock = 0; req_items[i].new_stock_qty = med->quantity; snprintf(req_items[i].error_msg, 100, "Insufficient '%s' (C%d). Has: %d, Req: %d.", req_items[i].name, req_items[i].code, med->quantity, req_items[i].quantity_requested); fprintf(stderr, " [FAIL] C%d (%s): Insufficient. Has %d, needs %d.\n", req_items[i].code, req_items[i].name, med->quantity, req_items[i].quantity_requested); valid = 0; } }
        req_items[i].stock_validation_done = 1; }
    if (!valid) { printf("<h3>Billing Errors</h3>"); for (int i = 0; i < n_items; i++) { if (strlen(req_items[i].error_msg) > 0) { printf("<p class='error'>%s</p>", req_items[i].error_msg); } } printf("<p><a href='../billing.html' class='btn'>Back</a></p>"); fflush(stdout); fprintf(stderr, "Billing abort: stock validation.\n"); return; }
    fprintf(stderr, "All validated. Update stock file.\n");
    // --- Start File Update Transaction ---
    FILE *fp_in = fopen(STOCK_FILE, "r"), *fp_out = fopen(TEMP_STOCK_FILE_BILLING, "w"); int file_error = 0;
    if (!fp_in || !fp_out) { fprintf(stderr, "FATAL: Cannot open files billing update! %s\n", strerror(errno)); printf("<p class='error'>Internal Error: files.</p>"); if(fp_in) fclose(fp_in); if(fp_out) fclose(fp_out); remove(TEMP_STOCK_FILE_BILLING); printf("<p><a href='../billing.html' class='btn'>Back</a></p>"); fflush(stdout); return; }
    char line[512], orig_line[512]; struct medicine m_line; int ln = 0;
    while (fgets(line, sizeof(line), fp_in)) { ln++; strcpy(orig_line, line); line[strcspn(line, "\r\n")] = 0; if (strspn(line, " \t")==strlen(line)) { if (fputs(orig_line, fp_out)==EOF) { file_error=1; break; } continue; }
        int line_code = 0; sscanf(line, "%*[^,],%d", &line_code); int billed_this = 0;
        for (int i = 0; i < n_items; i++) { if (req_items[i].code == line_code) { int items = sscanf(line, "%39[^,],%d,%49[^,],%lld,%f,%d,%d,%d,%d", m_line.name,&m_line.mcode,m_line.s_name,&m_line.s_contact,&m_line.price,&m_line.quantity,&m_line.year,&m_line.month,&m_line.day);
                 if (items!=9) { fprintf(stderr, "Parse err ln %d billing.\n", ln); file_error = 1; /* Mark error but try to continue to avoid inconsistent temp file */ break; }
                 if (fprintf(fp_out, "%s,%d,%s,%lld,%.2f,%d,%d,%d,%d\n", m_line.name,m_line.mcode,m_line.s_name,m_line.s_contact,m_line.price,req_items[i].new_stock_qty,m_line.year,m_line.month,m_line.day) < 0) { file_error=1; }
                 else { fprintf(stderr, " Updated C%d -> temp bill (New Qty: %d).\n", line_code, req_items[i].new_stock_qty); } billed_this = 1; break; } }
        if (!billed_this && !file_error) { if (fputs(orig_line, fp_out)==EOF) { file_error=1; } } if(ferror(fp_out) || file_error) { file_error=1; break; } }
    if(ferror(fp_in)) { file_error=1; } fclose(fp_in); if (fclose(fp_out)!=0) { file_error=1; }
    // --- End File Update Transaction Attempt ---

    if (file_error) { fprintf(stderr, "Billing fail: IO err file update. Clean temp.\n"); remove(TEMP_STOCK_FILE_BILLING); printf("<p class='error'>Internal file error updating stock. Aborted.</p>"); printf("<p><a href='../billing.html' class='btn'>Back</a></p>"); fflush(stdout); return; }
    else { // File write successful, attempt rename
        fprintf(stderr, "Replace stock file bill.\n"); if (remove(STOCK_FILE)!=0 && errno != ENOENT /* Don't fail if file didn't exist */) { fprintf(stderr, "CRIT: Err remove %s! %s\n", STOCK_FILE, strerror(errno)); remove(TEMP_STOCK_FILE_BILLING); stock_upd_ok=0; printf("<div class='error'>CRIT ERR: Cannot remove original stock file. Bill NOT processed, stock NOT updated.</div>\n"); }
        else if (rename(TEMP_STOCK_FILE_BILLING, STOCK_FILE)!=0) { fprintf(stderr, "CRIT: Err rename %s->%s! %s\n", TEMP_STOCK_FILE_BILLING, STOCK_FILE, strerror(errno)); stock_upd_ok=0; printf("<div class='error'>CRIT ERR: Cannot save updated stock file. Bill NOT processed, stock NOT updated. Data may be in '%s'.</div>\n", TEMP_STOCK_FILE_BILLING); }
        else { fprintf(stderr, "Stock file updated OK bill.\n"); stock_upd_ok = 1; }
    }

    // --- If Stock Update Successful, Update Memory and Save Sales ---
    if (stock_upd_ok) {
        fprintf(stderr, "Updating memory...\n"); int all_mem_ok = 1; for (int i = 0; i < n_items; i++) { int h_upd = updateHashTableQuantity(globalHashTable, globalHashTableSize, req_items[i].code, req_items[i].new_stock_qty); int b_upd = updateBstQuantity(globalBstRoot, req_items[i].code, req_items[i].new_stock_qty); if (!h_upd || !b_upd) { fprintf(stderr, "Warn: Mem update fail C%d (H:%d,B:%d)\n", req_items[i].code, h_upd, b_upd); all_mem_ok = 0; } }
         if (!all_mem_ok) { printf("<p class='warning' style='font-size:0.9em;'><i class='bi bi-exclamation-circle-fill'></i> Warn: Stock file OK, live view cache inconsistent.</p>"); }

        // Generate Invoice ID (Timestamp + Process ID for uniqueness)
        long current_time_secs = (long)time(NULL);
        pid_t current_pid = getpid();
        snprintf(generated_invoice_id, sizeof(generated_invoice_id), "%ld-%d", current_time_secs, current_pid);
        fprintf(stderr, "Generated Invoice ID: %s\n", generated_invoice_id);

        // Save Sales Record
        fprintf(stderr, "Saving sales records...\n"); struct sale_record sale; time_t t = time(NULL); struct tm tm = *localtime(&t); char date_s[11], time_s[9]; strftime(date_s, 11, "%Y-%m-%d", &tm); strftime(time_s, 9, "%H:%M:%S", &tm);
    
        for (int i = 0; i < n_items; i++) {
            strcpy(sale.invoice_id, generated_invoice_id); // Set the invoice ID for this sale item
            strcpy(sale.date_str, date_s);
            strcpy(sale.time_str, time_s);
            strncpy(sale.customer_name, cust_name, 49); sale.customer_name[49]='\0';
            sale.medicine_code = req_items[i].code;
            strncpy(sale.medicine_name, req_items[i].name, 39); sale.medicine_name[39]='\0';
            sale.quantity = req_items[i].quantity_requested;
            sale.price_per_item = req_items[i].price_per_item;
            sale.total_cost = sale.price_per_item * sale.quantity;
            if (saveSaleRecord(&sale)) { saved_count++; } else { fprintf(stderr, "Warn: Fail save sales C%d.\n", sale.medicine_code); }
        }
        sales_saved = (saved_count == n_items);
        if (!sales_saved) fprintf(stderr, "Warn: Only %d/%d sales saved.\n", saved_count, n_items); else fprintf(stderr, "All %d sales saved.\n", saved_count);

        // --- Generate HTML Bill Output ---
        printf("<div class='bill-details'>");
        printf("<h3>Bill Generated</h3>");
        printf("<p><strong>Invoice ID:</strong> %s</p>", generated_invoice_id); // Display Invoice ID
        time_t t_now = time(NULL); struct tm tm_now = *localtime(&t_now); printf("<p><strong>Date:</strong> %04d-%02d-%02d %02d:%02d:%02d</p>", tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        printf("<p><strong>Customer:</strong> %s</p>", cust_name);
        printf("<hr style='border-top: 1px dashed var(--accent); margin: 10px 0;'>");
        printf("<table style='width:100%%;margin-top:15px;border-collapse:collapse;font-size:0.95rem;'>");
        printf("<thead><tr style='background-color:var(--secondary);color:white;'><th style='padding:8px;text-align:left;'>Item</th><th style='padding:8px;text-align:right;'>Code</th><th style='padding:8px;text-align:right;'>Qty</th><th style='padding:8px;text-align:right;'>Price</th><th style='padding:8px;text-align:right;'>Total</th></tr></thead><tbody>");
        grand_total = 0.0;
        for (int i = 0; i < n_items; i++) {
            float line_total = req_items[i].price_per_item * req_items[i].quantity_requested; grand_total += line_total;
            printf("<tr><td style='padding:8px;border-bottom:1px dotted var(--accent);'>%s</td><td style='padding:8px;text-align:right;border-bottom:1px dotted var(--accent);'>%d</td><td style='padding:8px;text-align:right;border-bottom:1px dotted var(--accent);'>%d</td><td style='padding:8px;text-align:right;border-bottom:1px dotted var(--accent);'>₹%.2f</td><td style='padding:8px;text-align:right;border-bottom:1px dotted var(--accent);'>₹%.2f</td></tr>", req_items[i].name, req_items[i].code, req_items[i].quantity_requested, req_items[i].price_per_item, line_total);
        }
        printf("</tbody></table>");
        printf("<p class='bill-total' style='margin-top:20px;padding-top:15px;border-top:1px solid var(--accent);text-align:right;'><strong>Grand Total: ₹%.2f</strong></p>", grand_total);
        printf("<p style='font-size:0.9em;color:var(--status-active-text);'><i class='bi bi-check-circle-fill'></i> Stock file updated.</p>");
        if (sales_saved) { printf("<p style='font-size:0.9em;color:var(--status-active-text);'><i class='bi bi-journal-check'></i> Sales recorded.</p>"); } else { printf("<p class='error' style='font-size:0.9em;'><i class='bi bi-exclamation-triangle-fill'></i> Warn: Sales record save failed.</p>"); }
        printf("</div>");
        printf("<p style='margin-top: 20px; text-align:center;'><a href='../billing.html' class='btn btn-primary'>Generate Another Bill</a></p>");

    } else { // Stock update failed (rename failed after temp file write)
        fprintf(stderr, "Bill processing failed due to stock file persistence error. No sales recorded.\n");
        // Display error message already printed during the rename failure check.
        printf("<p><a href='../billing.html' class='btn'>Back</a></p>");
    }

    fflush(stdout); fprintf(stderr, "processBillingMultiple: Finished.\n"); fflush(stderr);
}


// checkExpiry remains unchanged...
void checkExpiry() {
    fprintf(stderr, "checkExpiry: Started.\n"); time_t now_t = time(NULL); struct tm now_tm = *localtime(&now_t); now_tm.tm_hour=0; now_tm.tm_min=0; now_tm.tm_sec=0; time_t today_start = mktime(&now_tm);
    const int warn_days = 90; struct tm warn_tm = now_tm; warn_tm.tm_mday += warn_days; mktime(&warn_tm); time_t warn_start = mktime(&warn_tm);
    printf("<h2>Stock Expiry Status</h2><p>Showing expired or expiring within %d days.</p>", warn_days); printf("<div class='table-container-box'><table class='expiry-table'><thead><tr><th>Name</th><th>Code</th><th>Expiry</th><th style='text-align: center;'>Status</th></tr></thead><tbody>"); fflush(stdout);
    int found = 0; if (globalBstRoot == NULL) { fprintf(stderr, "checkExpiry: BST empty.\n"); } else { checkExpiryRecursive(globalBstRoot, today_start, warn_start, &found, warn_days); }
    if (found == 0) { printf("<tr><td colspan='4' style='text-align:center; font-style:italic;'>No items expired or expiring soon.</td></tr>"); }
    printf("</tbody></table></div>"); fprintf(stderr, "checkExpiry: Finished.\n"); fflush(stdout);
}

// Modified generateReport to include Invoice ID
void generateReport() {
    fprintf(stderr, "generateReport (Detailed Table with Invoice ID): Called.\n");
    FILE *fp = fopen(SALES_FILE, "r");
    if (fp == NULL) {
        if (errno == ENOENT) { fprintf(stderr, "Sales file %s not found.\n", SALES_FILE); printf("<h2>Sales Report</h2><div class='report-summary'><p>No sales have been recorded yet.</p></div>"); }
        else { fprintf(stderr, "Error opening sales file %s: %s\n", SALES_FILE, strerror(errno)); printf("<h2>Error Generating Report</h2><p class='error'>Could not open sales history (%s). %s</p>", SALES_FILE, strerror(errno)); }
        fflush(stdout); return;
    }

    printf("<h2>Sales Report</h2>");

    // --- Detailed Sales Table ---
    printf("<div class='table-container-box' style='margin-bottom: 30px;'>"); // Add margin below table
    printf("<h2>Detailed Sales History</h2>");
    printf("<table class='stock-table'><thead>"); // Use stock-table style for consistency
    // Added Invoice ID column header
    printf("<tr><th>Invoice ID</th><th>Date</th><th>Time</th><th>Customer</th><th>Med Code</th><th>Med Name</th><th style='text-align:right;'>Qty</th><th style='text-align:right;'>Price/Item</th><th style='text-align:right;'>Total Cost</th></tr>");
    printf("</thead><tbody>");
    fflush(stdout);

    char line[512];
    char *field;
    char *line_ptr;
    int is_quoted;
    int line_num = 0;
    int data_found = 0;
    int is_header = 1;
    double total_sales_value = 0.0;
    long total_items_sold = 0;
    long transaction_count = 0; // Counts unique invoice IDs

    // To track unique invoices for summary
    char **unique_invoices = NULL;
    int unique_invoice_count = 0;
    int unique_invoice_capacity = 0;


    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        line[strcspn(line, "\r\n")] = 0; // Remove trailing newline/CR
        if (strspn(line, " \t") == strlen(line)) continue; // Skip blank lines

        if (is_header) {
             // Basic header check including InvoiceID
            if (strstr(line, "InvoiceID") && strstr(line, "Date") && strstr(line, "CustomerName") && strstr(line, "TotalCost")) {
                is_header = 0; continue; // Skip the header row
            } else {
                 fprintf(stderr, "generateReport: Warning - Sales file header might be missing or invalid (expected InvoiceID).\n");
                 is_header = 0; // Assume header missing, try processing this line
            }
        }

        // Use a temporary sale record struct to hold parsed data
        struct sale_record current_sale = {0};
        int field_index = 0;
        int parse_ok = 1;
        line_ptr = line; // Initialize pointer for get_csv_field

        while ((field = get_csv_field(&line_ptr, &is_quoted)) != NULL) {
            // Trim leading/trailing whitespace from field (important for conversions)
            char *start_trimmed = field;
            while (isspace((unsigned char)*start_trimmed)) start_trimmed++;
            char *end_trimmed = start_trimmed + strlen(start_trimmed) - 1;
            while (end_trimmed > start_trimmed && isspace((unsigned char)*end_trimmed)) end_trimmed--;
            *(end_trimmed + 1) = '\0';

            // Adjusted indices for InvoiceID at the start
            switch (field_index) {
                case 0: strncpy(current_sale.invoice_id, start_trimmed, sizeof(current_sale.invoice_id)-1); break;
                case 1: strncpy(current_sale.date_str, start_trimmed, sizeof(current_sale.date_str)-1); break;
                case 2: strncpy(current_sale.time_str, start_trimmed, sizeof(current_sale.time_str)-1); break;
                case 3: strncpy(current_sale.customer_name, start_trimmed, sizeof(current_sale.customer_name)-1); break;
                case 4: current_sale.medicine_code = atoi(start_trimmed); break;
                case 5: strncpy(current_sale.medicine_name, start_trimmed, sizeof(current_sale.medicine_name)-1); break;
                case 6: current_sale.quantity = atoi(start_trimmed); break;
                case 7: current_sale.price_per_item = atof(start_trimmed); break;
                case 8: current_sale.total_cost = atof(start_trimmed); break;
                default: break; // Ignore extra fields
            }
            field_index++;
        }

        // Basic validation: check if essential fields were parsed reasonably
        // Expecting 9 fields now. Check Invoice ID length > 0 as well.
        if (field_index >= 9 && strlen(current_sale.invoice_id) > 0 && current_sale.medicine_code > 0 && current_sale.quantity > 0 && current_sale.total_cost >= 0) {
            data_found = 1;

            // Count unique invoices for summary
            int found_invoice = 0;
            for(int i = 0; i < unique_invoice_count; ++i) {
                if (strcmp(unique_invoices[i], current_sale.invoice_id) == 0) {
                    found_invoice = 1;
                    break;
                }
            }
            if (!found_invoice) {
                if (unique_invoice_count >= unique_invoice_capacity) {
                    unique_invoice_capacity = (unique_invoice_capacity == 0) ? 10 : unique_invoice_capacity * 2;
                    char **temp_realloc = realloc(unique_invoices, unique_invoice_capacity * sizeof(char*));
                    if (!temp_realloc) {
                        fprintf(stderr, "generateReport: Error reallocating memory for unique invoices. Summary count may be incorrect.\n");
                        // Continue without tracking further unique invoices if realloc fails
                    } else {
                         unique_invoices = temp_realloc;
                    }
                }
                 if (unique_invoice_count < unique_invoice_capacity) { // Check again in case realloc failed
                    unique_invoices[unique_invoice_count] = strdup(current_sale.invoice_id);
                    if (!unique_invoices[unique_invoice_count]) {
                         fprintf(stderr, "generateReport: Error duplicating invoice ID string. Summary count may be incorrect.\n");
                    } else {
                        unique_invoice_count++;
                        transaction_count++; // Increment count only for unique invoices
                    }
                 }
            }

            total_items_sold += current_sale.quantity;
            total_sales_value += current_sale.total_cost;

            // Print the table row including Invoice ID
            printf("<tr>");
            printf("<td>%s</td>", current_sale.invoice_id); // Display Invoice ID
            printf("<td>%s</td>", current_sale.date_str);
            printf("<td>%s</td>", current_sale.time_str);
            printf("<td>%s</td>", current_sale.customer_name); // Consider HTML escaping if needed
            printf("<td>%d</td>", current_sale.medicine_code);
            printf("<td>%s</td>", current_sale.medicine_name); // Consider HTML escaping
            printf("<td style='text-align:right;'>%d</td>", current_sale.quantity);
            printf("<td style='text-align:right;'>₹%.2f</td>", current_sale.price_per_item); // Added Rupee Symbol
            printf("<td style='text-align:right;'>₹%.2f</td>", current_sale.total_cost);    // Added Rupee Symbol
            printf("</tr>\n");
            fflush(stdout);
        } else if (strlen(line) > 0 && !is_header) { // Avoid warning on blank lines or the actual header
             fprintf(stderr, "generateReport: Malformed or incomplete line %d in %s. Parsed %d fields. Skipping row.\n", line_num, SALES_FILE, field_index);
        }
    } // End while loop reading file

    if (ferror(fp)) {
        fprintf(stderr, "generateReport: Error reading %s: %s\n", SALES_FILE, strerror(errno));
        printf("<tr><td colspan='9' class='error'>Error reading sales data. Report may be incomplete.</td></tr>"); // Increased colspan
    }
    if (!data_found && !ferror(fp)) {
        printf("<tr><td colspan='9' style='text-align:center; font-style:italic;'>No sales data found in the file.</td></tr>"); // Increased colspan
    }

    fclose(fp);
    printf("</tbody></table></div>"); // Close table and container box

    // --- Summary Section ---
    printf("<div class='report-summary'>");
    printf("<h2>Sales Summary</h2>");
    if (transaction_count > 0) {
        printf("<ul>");
        printf("<li><strong>Total Unique Invoices (Transactions):</strong> %ld</li>", transaction_count); // Clarified meaning
        printf("<li><strong>Total Individual Items Sold:</strong> %ld</li>", total_items_sold);
        printf("<li><strong>Total Sales Value:</strong> ₹%.2f</li>", total_sales_value); // Added Rupee Symbol
        printf("</ul>");
    } else {
        printf("<p>No valid sales transactions found to summarize.</p>");
    }
    printf("</div>"); // Close report-summary

    // Free memory used for tracking unique invoices
    for(int i = 0; i < unique_invoice_count; ++i) {
        free(unique_invoices[i]);
    }
    free(unique_invoices);

    fprintf(stderr, "generateReport: Finished.\n");
    fflush(stdout);
}


// Modified searchMedicine to add Rupee symbol
void searchMedicine(char *request_data) {
    fprintf(stderr, "searchMedicine: Started.\n"); char *query = get_param(request_data, "searchQuery"); int matches = 0;
    if (!query || strlen(query) == 0) { printf("<p class='error'>No search term.</p><p><a href=\"medical.exe\" class='btn'>View All</a></p>"); if (query) free(query); return; }
    int code = 0; int is_code = 0; char *e; errno = 0; long pcode = strtol(query, &e, 10); int is_num = (errno==0 && e!=query && pcode>=INT_MIN && pcode<=INT_MAX); while (is_num && isspace((unsigned char)*e)) e++;
    if (is_num && *e=='\0' && pcode>0) { code=(int)pcode; is_code=1; fprintf(stderr, "Search: Code query %d\n", code); } else { fprintf(stderr, "Search: Name query '%s'\n", query); }
    printf("<div style='overflow-x:auto;'><table class='stock-table'><thead><tr><th>Code</th><th>Name</th><th>Supplier</th><th>Contact</th><th>Price</th><th>Quantity</th><th>Expiry Date</th></tr></thead><tbody>"); fflush(stdout);
    if (is_code) { fprintf(stderr, "Search hash code %d\n", code); struct medicine* med = searchHashTableByCode(globalHashTable, globalHashTableSize, code);
        if (med != NULL) { matches=1; struct medicine m = *med;
            // Added Rupee symbol below
            printf("<tr><td>%d</td><td>%s</td><td>%s</td><td>%lld</td><td>₹%.2f</td><td style='text-align:center;'>%d</td><td>%04d-%02d-%02d</td></tr>\n", m.mcode, m.name, m.s_name, m.s_contact, m.price, m.quantity, m.year, m.month, m.day); fflush(stdout); }
        else { fprintf(stderr, "Code %d not found hash.\n", code); } }
    else { fprintf(stderr, "Search BST name '%s'\n", query); if (globalBstRoot == NULL) { fprintf(stderr, "BST empty, cannot search name.\n"); }
           else { searchBSTByNameSubstring(globalBstRoot, query, &matches); } // This function already modified for Rupee symbol
    }
    if (matches == 0) { printf("<tr><td colspan='7' style='text-align:center; font-style:italic;'>No match found for '%s'.</td></tr>", query); }
    printf("</tbody></table></div>"); printf("<p style=\"margin-top: 20px; text-align:center;\"><a href=\"medical.exe\" class=\"btn btn-secondary\">View All Stock</a></p>"); fflush(stdout);
    if (query) free(query); fprintf(stderr, "searchMedicine: Finished.\n"); fflush(stderr);
}


// --- Main Function (Simplified Routing Logic) ---
int main() {
    // Seed random number generator for potential use (like invoice ID)
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    fprintf(stderr, "\n--------------------\nmedical.exe: Started (HS/BST).\n"); fflush(stderr);
    char *req_method = NULL, *req_data = NULL, *q_string = NULL, *len_s = NULL;
    char *action = NULL, *actionType = NULL; long data_len = 0; int processed = 0, loaded_ok = 0;

    globalHashTable = createHashTable(globalHashTableSize); globalBstRoot = NULL;
    if (globalHashTable == NULL) { printf("Content-Type: text/html\n\n<!DOCTYPE html><html><body><h1>Internal Error</h1><p class='error'>Hash Table init failed.</p></body></html>"); fprintf(stderr, "FATAL: Hash table alloc failed.\n"); return 1; }
    loaded_ok = loadStockData(STOCK_FILE, &globalHashTable, &globalHashTableSize, &globalBstRoot);
    if (!loaded_ok) { printf("Content-Type: text/html\n\n<!DOCTYPE html><html><body><h1>Internal Error</h1><p class='error'>Failed load stock data from '%s'.</p></body></html>", STOCK_FILE); fprintf(stderr, "FATAL: loadStockData failed.\n"); freeHashTable(globalHashTable, globalHashTableSize); freeTree(globalBstRoot); return 1; }

    printf("Content-Type: text/html\n\n"); fflush(stdout);
    printf("<!DOCTYPE html><html lang=\"en\"><head>");
    printf("<meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">");
    printf("<title>Discount Pharmacy - Management (HS/BST)</title>");
    printf("<link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css\">");
    printf("<link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/npm/bootstrap-icons/font/bootstrap-icons.css\">");
    printf("<link rel=\"icon\" href=\"../discount pharmacy.png\" type=\"image/x-icon\">");
    printf("<style>"); // --- Embed All Shared CSS Rules ---
    // CSS rules remain unchanged...
    printf(":root{--primary:#1976D2;--secondary:#64B5F6;--accent:#BBDEFB;--text:#2d3748;--light:#fff;--bg-gradient:linear-gradient(135deg,#1976D2 0%%,#64B5F6 100%%);--navbar-bg:rgba(255,255,255,.95);--card-bg:rgba(255,255,255,.9);--table-border:rgba(187,222,251,.6);--table-header-bg:var(--primary);--table-header-text:var(--light);--table-row-hover:rgba(187,222,251,.3);--status-expired-bg:#FEE2E2;--status-expired-text:#B91C1C;--status-warning-bg:#FEF3C7;--status-warning-text:#B45309;--status-active-bg:#D1FAE5;--status-active-text:#047857;--status-active-default:var(--text)}");
    printf("*{margin:0;padding:0;box-sizing:border-box}body{background:var(--bg-gradient);color:var(--text);font-family:'Poppins','Segoe UI',Tahoma,Geneva,Verdana,sans-serif;min-height:100vh;overflow-x:hidden;position:relative;background-attachment:fixed}");
    printf(".bg-circles{position:fixed;top:0;left:0;width:100%%;height:100%%;z-index:-2;overflow:hidden;pointer-events:none}.circle{position:absolute;border-radius:50%%;background:rgba(255,255,255,.08);animation:float 20s infinite ease-in-out alternate}.circle-1{width:300px;height:300px;top:-100px;left:-100px;animation-duration:25s}.circle-2{width:400px;height:400px;bottom:-150px;right:-150px;animation-duration:30s;animation-delay:2s}.circle-3{width:200px;height:200px;top:25%%;right:15%%;animation-duration:20s;animation-delay:1s}@keyframes float{0%%{transform:translateY(0) scale(1)}100%%{transform:translateY(-20px) scale(1.05)}}");
    printf(".navbar{background-color:var(--navbar-bg);box-shadow:0 4px 30px rgba(0,0,0,.1);backdrop-filter:blur(5px);border-bottom:1px solid rgba(255,255,255,.3);display:flex;align-items:center;padding:25px 40px;position:sticky;top:0;z-index:100}.logo a{display:flex;align-items:center;text-decoration:none;color:var(--primary);font-weight:700;font-size:20px;transition:all .3s ease;flex-shrink:0}.logo a:hover{transform:scale(1.05)}.logo img{height:35px;width:35px;margin-right:10px}.nav-links{display:flex;flex-wrap:wrap;gap:15px 20px;margin-left:auto}.nav-links a{text-decoration:none;color:var(--text);font-weight:500;padding:8px 16px;border-radius:30px;transition:all .3s ease;position:relative;font-size:1.1rem}.nav-links a:after{content:'';position:absolute;width:0;height:2px;bottom:-2px;left:50%%;background:var(--primary);transition:all .3s ease;transform:translateX(-50%%)}.nav-links a:hover{color:var(--primary)}.nav-links a:hover:after{width:70%%}");
    printf(".user-menu{position:relative;margin-left:20px;flex-shrink:0}.user-icon{width:40px;height:40px;background:var(--bg-gradient);border-radius:50%%;display:flex;align-items:center;justify-content:center;color:#fff;cursor:pointer;box-shadow:0 4px 10px rgba(25,118,210,.3);transition:all .3s ease}.user-icon i{font-size:1.3rem;line-height:1}.user-icon:hover{transform:scale(1.1)}.dropdown-card{position:absolute;right:0;top:55px;background:#fff;border-radius:10px;box-shadow:0 10px 30px rgba(0,0,0,.1);padding:10px;min-width:120px;opacity:0;visibility:hidden;transform:translateY(-10px);transition:all .3s ease;z-index:110}.user-menu:hover .dropdown-card,.user-menu:focus-within .dropdown-card{opacity:1;visibility:visible;transform:translateY(0)}.logout-btn{display:flex;align-items:center;gap:8px;padding:10px 15px;color:#e53e3e;text-decoration:none;font-weight:500;border-radius:8px;transition:all .3s ease}.logout-btn i{font-size:1rem}.logout-btn:hover{background-color:#fed7d7}");
    printf(".page-content{display:flex;flex-direction:column;align-items:center;padding:40px 20px;z-index:1;position:relative;width:100%%}");
    printf("h2.page-title{color:#fff;font-size:36px;font-weight:700;text-align:center;margin:30px 0 40px 0;text-shadow:0 2px 10px rgba(0,0,0,.2);letter-spacing:1px}");
    printf(".search-container{display:flex;justify-content:center;margin-bottom:30px;width:100%%;max-width:600px}.search-container form{display:flex;width:100%%}.search-container input[type=search]{flex-grow:1;padding:10px 15px;font-size:1rem;border:1px solid var(--accent);border-right:none;border-radius:8px 0 0 8px;background-color:rgba(255,255,255,.8);color:var(--text);transition:border-color .3s ease,box-shadow .3s ease;outline:none}.search-container input[type=search]:focus{border-color:var(--primary);box-shadow:0 0 0 3px rgba(25,118,210,.2);z-index:2;position:relative}.search-container button{padding:10px 15px;border:1px solid var(--accent);background-color:var(--light);color:var(--primary);border-radius:0 8px 8px 0;cursor:pointer;transition:background-color .3s ease,color .3s ease;flex-shrink:0;display:flex;align-items:center;justify-content:center}.search-container button:hover{background-color:var(--accent);color:var(--primary)}.search-container button i{font-size:1.2rem}");
    printf(".table-container-box{background:var(--card-bg);backdrop-filter:blur(10px);border-radius:20px;box-shadow:0 15px 30px rgba(0,0,0,.2);border:1px solid rgba(255,255,255,.5);padding:30px 35px;max-width:1100px;width:95%%;margin:0 auto 40px auto;z-index:2;overflow-x:auto}.table-container-box h2{color:var(--primary);font-size:28px;margin-top:0;margin-bottom:25px;text-shadow:none;text-align:center}");
    printf(".error{color:#D8000C;background-color:#FFD2D2;border:1px solid #D8000C;margin:10px 0;padding:15px;border-radius:4px}.success{color:#4F8A10;background-color:#DFF2BF;border:1px solid #4F8A10;margin:10px 0;padding:15px;border-radius:4px}.warning{color:#9F6000;background-color:#FEEFB3;border:1px solid #9F6000;margin:10px 0;padding:15px;border-radius:4px}");
    printf(".stock-table{width:100%%;border-collapse:collapse;margin-top:15px;color:var(--text);font-size:.95rem}.stock-table th,.stock-table td{border:1px solid var(--table-border);padding:12px 15px;text-align:left;vertical-align:middle}.stock-table th{background-color:var(--table-header-bg);color:var(--table-header-text);font-weight:600;text-transform:uppercase;letter-spacing:.5px}.stock-table tbody tr:hover td{background-color:var(--table-row-hover) !important;}");
    printf(".expiry-table{width:100%%;border-collapse:collapse;margin-top:15px;color:var(--text);font-size:.95rem}.expiry-table th,.expiry-table td{border:1px solid var(--table-border);padding:12px 15px;text-align:left;vertical-align:middle}.expiry-table th{background-color:var(--table-header-bg);color:var(--table-header-text);font-weight:600;text-transform:uppercase;letter-spacing:.5px}");
    printf(".status-cell{font-weight:600;text-align:center;border-radius:15px;padding:5px 10px;display:inline-block;min-width:100px;line-height:1.2;}");
    printf("tr.status-expired td{background-color:var(--status-expired-bg);}td span.status-expired{color:var(--status-expired-text);border:1px solid var(--status-expired-text);}");
    printf("tr.status-warning td{background-color:var(--status-warning-bg);}td span.status-warning{color:var(--status-warning-text);border:1px solid var(--status-warning-text);}");
    printf("tr.status-active td{}td span.status-active{color:var(--status-active-default);border:1px solid #ccc;}");
    printf(".expiry-table tbody tr:hover td{background-color:var(--table-row-hover) !important;color:var(--text) !important;}.expiry-table tbody tr:hover td span.status-cell{color:var(--text) !important;border-color:var(--text) !important;background-color:transparent !important;}");
    printf(".bill-details{border:1px solid var(--accent);padding:25px;margin-top:20px;border-radius:15px;background-color:rgba(255,255,255,.9);box-shadow:0 10px 25px rgba(0,0,0,.1);backdrop-filter:blur(5px);max-width:700px;width:95%%;margin-left:auto;margin-right:auto;}.bill-details h3{color:var(--primary);margin-bottom:20px;border-bottom:1px solid var(--accent);padding-bottom:15px;text-align:center;font-size:1.6rem;}.bill-details p{margin-bottom:10px;line-height:1.6;font-size:1rem;}.bill-details strong{color:var(--text);font-weight:600;}.bill-total{font-weight:bold;font-size:1.2rem;margin-top:20px;padding-top:15px;border-top:1px solid var(--accent);text-align:right;}");
    printf(".report-summary{background:var(--card-bg);backdrop-filter:blur(10px);border-radius:15px;box-shadow:0 10px 25px rgba(0,0,0,.1);border:1px solid rgba(255,255,255,.5);padding:25px 35px;max-width:700px;width:95%%;margin:20px auto;z-index:2;}.report-summary ul{list-style:none;padding:0;}.report-summary li{font-size:1.1rem;margin-bottom:12px;padding-bottom:12px;border-bottom:1px dashed var(--accent);}.report-summary li:last-child{border-bottom:none;margin-bottom:0;padding-bottom:0;}.report-summary strong{color:var(--primary);}");
    printf(".btn{display:inline-block;font-weight:400;color:#212529;text-align:center;vertical-align:middle;user-select:none;background-color:transparent;border:1px solid transparent;padding:.375rem .75rem;font-size:1rem;line-height:1.5;border-radius:.25rem;transition:color .15s ease-in-out,background-color .15s ease-in-out,border-color .15s ease-in-out,box-shadow .15s ease-in-out}.btn-primary{color:#fff;background-color:#1976D2;border-color:#1976D2}.btn-primary:hover{color:#fff;background-color:#1565C0;border-color:#115293}.btn-secondary{color:#fff;background-color:#6c757d;border-color:#6c757d}.btn-secondary:hover{color:#fff;background-color:#5a6268;border-color:#545b62}.btn-info{color:#fff;background-color:#0dcaf0;border-color:#0dcaf0}.btn-info:hover{color:#fff;background-color:#0baccc;border-color:#0aa1bf}");
    printf("</style></head><body>");
    printf("<div class=\"bg-circles\"><div class=\"circle circle-1\"></div><div class=\"circle circle-2\"></div><div class=\"circle circle-3\"></div></div>");
    printf("<header><nav class=\"navbar\">"); // Navbar
    printf("<div class=\"logo\"><a href=\"../medical shop.html\"><img src=\"../discount pharmacy.png\" alt=\"Logo\"><span>DISCOUNT PHARMACY</span></a></div>");
    printf("<div class=\"nav-links\"><a href=\"../medical shop.html\">Home</a><a href=\"medical.exe?action=generate_report\">Reports</a><a href=\"medical.exe?action=check_expiry\">Expiry</a></div>");
    printf("<div class=\"user-menu\" tabindex=\"0\"><div class=\"user-icon\"><i class=\"bi bi-person-fill\"></i></div><div class=\"dropdown-card\"><a href=\"../login.html\" class=\"logout-btn\"><i class=\"bi bi-box-arrow-right\"></i> Logout</a></div></div>");
    printf("</nav></header>");
    printf("<main class=\"page-content\">"); fflush(stdout); // Main Content Start

    // --- Get Request Data ---
    // Request data fetching remains unchanged...
    req_method = getenv("REQUEST_METHOD"); if (req_method == NULL) req_method = "GET";
    fprintf(stderr, "Method: %s\n", req_method); req_data = NULL;
    if (strcmp(req_method, "POST") == 0) { len_s = getenv("CONTENT_LENGTH"); if (len_s != NULL) { errno = 0; data_len = strtol(len_s, NULL, 10);
            if (errno == 0 && data_len > 0 && data_len <= 20*1024*1024) { req_data = (char *)malloc(data_len + 1); if (req_data) { size_t rd = fread(req_data, 1, data_len, stdin); if (rd==(size_t)data_len) { req_data[data_len]='\0'; fprintf(stderr, "Read %ld POST\n", data_len); } else { free(req_data); req_data = NULL; fprintf(stderr, "POST read err (%zu/%ld)\n", rd, data_len); } } else { fprintf(stderr, "Malloc fail POST %ld\n", data_len); } }
            else if (data_len > 20*1024*1024) { fprintf(stderr, "POST too large: %ld\n", data_len); } else { fprintf(stderr, "Bad CONTENT_LENGTH: %s\n", len_s); } } else { fprintf(stderr, "No CONTENT_LENGTH POST\n"); } }
    else if (strcmp(req_method, "GET") == 0) { q_string = getenv("QUERY_STRING"); if (q_string != NULL && strlen(q_string) > 0) { req_data = strdup(q_string); if (!req_data) fprintf(stderr, "strdup fail GET\n"); else fprintf(stderr, "GET data: %s\n", req_data); } else { fprintf(stderr, "No QUERY_STRING GET\n"); } }

    // --- Routing ---
    // Routing logic remains unchanged...
    if (req_data != NULL) { action = get_param(req_data, "action"); if (action == NULL) { actionType = get_param(req_data, "actionType"); } }
    if (action != NULL) { fprintf(stderr, "Route action='%s'\n", action);
        if (strcmp(action, "add_stock") == 0 && strcmp(req_method, "POST") == 0) { printf("<h2 class='page-title'>Add Stock Results</h2>"); processAddStock(req_data); processed = 1; }
        else if (strcmp(action, "update_stock") == 0 && strcmp(req_method, "POST") == 0) { printf("<h2 class='page-title'>Update Stock Results</h2>"); processUpdateStock(req_data); processed = 1; }
        else if (strcmp(action, "billing") == 0 && strcmp(req_method, "POST") == 0) { printf("<h2 class='page-title'>Billing Results</h2>"); processBillingMultiple(req_data); processed = 1; }
        else if (strcmp(action, "generate_report") == 0 && strcmp(req_method, "GET") == 0) { generateReport(); processed = 1; } // generateReport prints its own title
        else if (strcmp(action, "check_expiry") == 0 && strcmp(req_method, "GET") == 0) { checkExpiry(); processed = 1; } // checkExpiry prints its own title
        else { fprintf(stderr, "Unknown action/method: %s (%s)\n", action, req_method); printf("<h2 class='page-title'>Error</h2><div class='error'>Invalid action ('%s')/method.</div>\n", action ? action : "NULL"); processed = 1; }
        free(action); }
    else if (actionType != NULL) { fprintf(stderr, "Route actionType='%s'\n", actionType);
         if (strcmp(actionType, "searchStock") == 0 && req_data != NULL ) { printf("<h2 class='page-title'>Stock Search Results</h2>"); printf("<div class=\"search-container\"><form action=\"medical.exe\" method=\"post\" class=\"d-flex w-100\"><input class=\"form-control\" type=\"search\" placeholder=\"Search...\" name=\"searchQuery\" required><input type=\"hidden\" name=\"actionType\" value=\"searchStock\"><button class=\"btn btn-primary\" type=\"submit\"><i class=\"bi bi-search\"></i></button></form></div>"); fflush(stdout); searchMedicine(req_data); processed = 1; }
         else { fprintf(stderr, "Unknown actionType: %s\n", actionType); printf("<h2 class='page-title'>Error</h2><div class='error'>Invalid action type ('%s').</div>\n", actionType); processed = 1; }
        free(actionType); }

    if (!processed) { // Default Action: View Stock
        fprintf(stderr, "Default action: viewStock.\n"); printf("<h2 class='page-title'>Pharmacy Stock</h2>");
        printf("<div class=\"search-container\"><form action=\"medical.exe\" method=\"post\" class=\"d-flex w-100\"><input class=\"form-control\" type=\"search\" placeholder=\"Search stock...\" name=\"searchQuery\" required><input type=\"hidden\" name=\"actionType\" value=\"searchStock\"><button class=\"btn btn-primary\" type=\"submit\"><i class=\"bi bi-search\"></i></button></form></div>");
        printf("<div class=\"table-container-box\"><h2>Current Stock Levels</h2>"); fflush(stdout); viewStock(); printf("</div>"); // viewStock now shows Rupee symbol
    }

    printf("</main></body></html>"); fflush(stdout); // End HTML

    // --- Cleanup ---
    if (req_data) free(req_data);
    fprintf(stderr, "Freeing memory...\n"); fflush(stderr);
    freeHashTable(globalHashTable, globalHashTableSize); freeTree(globalBstRoot); globalHashTable = NULL; globalBstRoot = NULL;
    fprintf(stderr, "medical.exe: Finished.\n--------------------\n\n"); fflush(stderr); return 0;
} // END main