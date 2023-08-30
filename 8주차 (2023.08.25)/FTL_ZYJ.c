#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define TOTAL_BLOCKS 256
#define PAGES_PER_BLOCK 16
#define PAGE_SIZE 4096
#define GCTrigger 0.6

typedef struct {
    char data[PAGE_SIZE];
} Page;

typedef struct {
    Page pages[PAGES_PER_BLOCK];
    int valid[PAGES_PER_BLOCK]; 
} Block;

Block flash[TOTAL_BLOCKS];
Block GCBuffer;

int L2P[TOTAL_BLOCKS][PAGES_PER_BLOCK]; 
int P2L[TOTAL_BLOCKS]; 
int invalidPagesCounter[TOTAL_BLOCKS];
int GCMappingSupporter[PAGES_PER_BLOCK]; // GC에 사용되는 MAPPING 기록 SUPPORTER

void initFTL() { 
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        P2L[i] = -1;
        for (int j = 0; j < PAGES_PER_BLOCK; j++) {
            L2P[i][j] = -1;
            flash[i].valid[j] = 0;
        }
        invalidPagesCounter[i] = PAGES_PER_BLOCK;
    }
}

int allocateBlock() { 
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        if (L2P[i][0] == -1 && P2L[i] == -1) {
            return i;
        }
    }
    return -1;
}

int invalidPagesInBlock(int block) { 
    int count = 0;
    for (int j = 0; j < PAGES_PER_BLOCK; j++) {
        if (flash[block].valid[j] == 0) {
            count++;
        }
    }
    return count;
}

void initinvalidPagesCounter(){
    for (int i = 0; i < TOTAL_BLOCKS; i++){
        invalidPagesCounter[i] = invalidPagesInBlock(i);
    }
}

void eraseBlock(int physicalBlock) {
    if (physicalBlock != -1) {
        int logicalBlock = P2L[physicalBlock];
        for (int j = 0; j < PAGES_PER_BLOCK; j++) {
            memset(flash[physicalBlock].pages[j].data, 0, PAGE_SIZE);
            flash[physicalBlock].valid[j] = 0;
            invalidPagesCounter[physicalBlock] = PAGES_PER_BLOCK;
            L2P[logicalBlock][j] = -1;
        }
        P2L[physicalBlock] = -1;
    }
}

// Page merge GC (only 2 blocks supported)
void moveValidPagesToBuffer(int targetBlock, int *l) {
    for (int k = 0; k < PAGES_PER_BLOCK; k++) {
        if (flash[targetBlock].valid[k]) {
            memcpy(GCBuffer.pages[*l].data, flash[targetBlock].pages[k].data, PAGE_SIZE);
            GCMappingSupporter[*l] = k;
            (*l)++;
        }
    }
}

void garbageCollector_forTheWorst() {
    int HtargetBlock = -1;
    int LtargetBlock = -1;
    int MaxInvalidPages = -1;
    int RemainingPages = 0;

    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        int currentInvalidPages = invalidPagesCounter[i];
        if (currentInvalidPages > GCTrigger * PAGES_PER_BLOCK) {
            HtargetBlock = i;
            MaxInvalidPages = currentInvalidPages;
            RemainingPages = PAGES_PER_BLOCK - MaxInvalidPages;
        }
    }
    bool foundLtargetBlock = false;
    for (int j = 0; j < TOTAL_BLOCKS && !foundLtargetBlock; j++) {
        int currentInvalidPages = invalidPagesCounter[j];
        if (currentInvalidPages == RemainingPages) {
            LtargetBlock = j;
            foundLtargetBlock = true;
        } else if (currentInvalidPages < RemainingPages && currentInvalidPages > 0 && LtargetBlock == -1) {
            LtargetBlock = j;
        }
    }

    if (HtargetBlock != -1 && LtargetBlock != -1) {
        int l = 0;
        moveValidPagesToBuffer(HtargetBlock, &l);
        int L = l;
        moveValidPagesToBuffer(LtargetBlock, &l);
        
        eraseBlock(P2L[HtargetBlock]);
        eraseBlock(P2L[LtargetBlock]);
        int GCnewPhysicalBlock = allocateBlock();
        
        for (int k = 0; k < l; k++) {
            memcpy(flash[GCnewPhysicalBlock].pages[k].data, GCBuffer.pages[k].data, PAGE_SIZE);
            flash[GCnewPhysicalBlock].valid[k] = 1;
            if (k < L) {
                L2P[P2L[HtargetBlock]][GCMappingSupporter[k]] = GCnewPhysicalBlock;
            } else {
                L2P[P2L[LtargetBlock]][GCMappingSupporter[k]] = GCnewPhysicalBlock;
            }
        }
        invalidPagesCounter[GCnewPhysicalBlock] = PAGES_PER_BLOCK - l;
    }
}

void readPage(int logicalBlock, int page, char *buffer) {
    int physicalBlock = L2P[logicalBlock][page];
    if (physicalBlock != -1 && flash[physicalBlock].valid[page]) {
    memcpy(buffer, flash[physicalBlock].pages[page].data, PAGE_SIZE);
    } 
    else{
    memset(buffer, 0, PAGE_SIZE);
    }
}

void writePage(int logicalBlock, int page, char *buffer) {
    int physicalBlock = L2P[logicalBlock][page];
    if (physicalBlock == -1) {
        physicalBlock = allocateBlock();
        if (physicalBlock == -1) {
            garbageCollector_forTheWorst();
            physicalBlock = allocateBlock();
        }
        L2P[logicalBlock][page] = physicalBlock;
        P2L[physicalBlock] = logicalBlock;
    }

    if (flash[physicalBlock].valid[page] == 0) {
        memcpy(flash[physicalBlock].pages[page].data, buffer, PAGE_SIZE);
        flash[physicalBlock].valid[page] = 1;
        invalidPagesCounter[physicalBlock]--;
    } else {
        int newPhysicalBlock = allocateBlock();
        flash[physicalBlock].valid[page] = 0;
        invalidPagesCounter[physicalBlock]++;
        if (newPhysicalBlock == -1) {
            garbageCollector_forTheWorst();
            newPhysicalBlock = allocateBlock();
        }
        memcpy(GCBuffer.pages[page].data, buffer, PAGE_SIZE);
        GCBuffer.valid[page] = 1;
        for (int j = 0; j < PAGES_PER_BLOCK; j++) {
            if (flash[physicalBlock].valid[j]) {
                memcpy(GCBuffer.pages[j].data, flash[physicalBlock].pages[j].data, PAGE_SIZE);
                GCBuffer.valid[j] = 1;
            }
        }
        for (int j = 0; j < PAGES_PER_BLOCK; j++){
            if (GCBuffer.valid[j]){
                memcpy(flash[newPhysicalBlock].pages[j].data, GCBuffer.pages[j].data, PAGE_SIZE);
                flash[newPhysicalBlock].valid[j] = 1;
                invalidPagesCounter[newPhysicalBlock]--;
            }
        }
        eraseBlock(physicalBlock);
        L2P[logicalBlock][page] = newPhysicalBlock;
        P2L[newPhysicalBlock] = logicalBlock;
    }
}

int main() {
    initFTL();
    initinvalidPagesCounter();

    char buffer[PAGE_SIZE] = "Hello";
    char readBuffer[PAGE_SIZE];
    
    writePage(10, 5, buffer);
    readPage(10, 5, readBuffer);
    printf("Read data: %s\n", readBuffer);
    printf("Physical block: %d\n", L2P[10][5]);


    strcpy(buffer, "World");
    writePage(10, 5, buffer);
    readPage(10, 5, readBuffer);
    printf("Read data: %s\n", readBuffer);
    printf("Physical block: %d\n", L2P[10][5]);

    return 0;
}
