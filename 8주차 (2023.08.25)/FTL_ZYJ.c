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

typedef struct {
    int PBN;
    int PPN;
} PhysicalLocation;

typedef struct {
    char data[PAGE_SIZE];
    int LPN;
    bool iswritten;
} WBuffer;

Block flash[TOTAL_BLOCKS];
Block GCBuffer; // GC를 위한 buffer
WBuffer writeBuffer[PAGES_PER_BLOCK]; // write buffer


PhysicalLocation L2P[TOTAL_BLOCKS * PAGES_PER_BLOCK]; // page mapping table
int P2L[TOTAL_BLOCKS][PAGES_PER_BLOCK]; 
int invalidPagesCounter[TOTAL_BLOCKS]; // block 내의 invalid page 수를 저장하는 배열
int GCMappingSupporter[PAGES_PER_BLOCK]; 
int GCBufferLogicalMapping[PAGES_PER_BLOCK * 2]; 
int GCBufferPageCount[TOTAL_BLOCKS];
int updateCounter[PAGES_PER_BLOCK];
int n = 0; // write buffer에 들어있는 page 수를 세는 변수

// FTL초기화
void initFTL() {
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        for (int j = 0; j < PAGES_PER_BLOCK; j++) {
            flash[i].valid[j] = 0;
            P2L[i][j] = -1;  // Corrected this line
        }
        invalidPagesCounter[i] = PAGES_PER_BLOCK;
    }
    for (int i = 0; i < TOTAL_BLOCKS * PAGES_PER_BLOCK; i++) {
        L2P[i].PBN = -1;
        L2P[i].PPN = -1;
    }
    for (int i = 0; i < PAGES_PER_BLOCK * 2; i++) {
        GCMappingSupporter[i] = -1;
        GCBufferLogicalMapping[i] = -1;
    }
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        GCBufferPageCount[i] = 0;
    }
}

// Block 분배함수 (physical block만 분배)
int allocatePhysicalBlock() {
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        if (invalidPagesCounter[i] == PAGES_PER_BLOCK) {
            return i;
        }
    }
    return -1;
}

// Block 내의 invalid page 수를 계산하는 함수
int invalidPagesInBlock(int block) { 
    int count = 0;
    for (int j = 0; j < PAGES_PER_BLOCK; j++) {
        if (flash[block].valid[j] == 0) {
            count++;
        }
    }
    return count;
}

// invalidPagesCounter 초기화
void initinvalidPagesCounter(){
    for (int i = 0; i < TOTAL_BLOCKS; i++){
        invalidPagesCounter[i] = invalidPagesInBlock(i);
    }
}

// Block을 erase하는 함수(physical block만 erase)
void eraseBlock(int physicalBlock) {
    if (physicalBlock != -1) {
        for (int j = 0; j < PAGES_PER_BLOCK; j++) {
            int logicalPage = P2L[physicalBlock][j];
            memset(flash[physicalBlock].pages[j].data, 0, PAGE_SIZE);
            flash[physicalBlock].valid[j] = 0;
            if(logicalPage != -1) {  // logicalPage가 존재하는지 확인
                L2P[logicalPage].PBN = -1;
                L2P[logicalPage].PPN = -1;
            }
            P2L[physicalBlock][j] = -1;
        }
        invalidPagesCounter[physicalBlock] = PAGES_PER_BLOCK;
    }
}

// Page merge GC (only 2 blocks supported)
void moveValidPagesToBuffer(int targetBlock, int *l) {
    for (int k = 0; k < PAGES_PER_BLOCK; k++) {
        if (flash[targetBlock].valid[k] && L2P[P2L[targetBlock][k]].PBN == targetBlock) {
            memcpy(GCBuffer.pages[*l].data, flash[targetBlock].pages[k].data, PAGE_SIZE);
            GCMappingSupporter[*l] = k;
            GCBufferLogicalMapping[*l] = P2L[targetBlock][k];
            (*l)++;
        }
    }
    GCBufferPageCount[targetBlock] = *l;
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
        moveValidPagesToBuffer(LtargetBlock, &l);

        eraseBlock(HtargetBlock);
        eraseBlock(LtargetBlock);
        int GCnewPhysicalBlock = allocatePhysicalBlock(); 
        
        for (int k = 0; k < l; k++) {
            memcpy(flash[GCnewPhysicalBlock].pages[k].data, GCBuffer.pages[k].data, PAGE_SIZE);
            flash[GCnewPhysicalBlock].valid[k] = 1;
            int logicalPage = GCBufferLogicalMapping[k];
            L2P[logicalPage].PBN = GCnewPhysicalBlock;
            L2P[logicalPage].PPN = k;
            P2L[GCnewPhysicalBlock][k] = logicalPage; 
        }
        invalidPagesCounter[GCnewPhysicalBlock] = PAGES_PER_BLOCK - l;
    }
}

void readPage(int logicalBlock, char *buffer) {
    for (int i = 0; i < PAGES_PER_BLOCK; i++) {
        if (writeBuffer[i].LPN == logicalBlock) {
            memcpy(buffer, writeBuffer[i].data, PAGE_SIZE);
            return;
        }
    }
    int physicalBlock = L2P[logicalBlock].PBN;
    int page = L2P[logicalBlock].PPN;
    if (physicalBlock != -1 && page != -1 && flash[physicalBlock].valid[page]) {
        memcpy(buffer, flash[physicalBlock].pages[page].data, PAGE_SIZE);
    } 
    else{
        memset(buffer, 0, PAGE_SIZE);
    }
}

void writePagetoFlash() {
    int newPhysicalBlock = allocatePhysicalBlock();
    if (allocatePhysicalBlock() == -1) {
        garbageCollector_forTheWorst();
        newPhysicalBlock = allocatePhysicalBlock();
    }
    for (int i = 0; i < PAGES_PER_BLOCK; i++) {
        memcpy(flash[newPhysicalBlock].pages[i].data, writeBuffer[i].data, PAGE_SIZE);
        flash[newPhysicalBlock].valid[i] = 1;
        L2P[writeBuffer[i].LPN].PBN = newPhysicalBlock;
        L2P[writeBuffer[i].LPN].PPN = i;
        if (updateCounter[i]) {
            invalidPagesCounter[newPhysicalBlock]++;
        }
        writeBuffer[i].iswritten = false; // buffer에 있는 data를 flash에 write했으므로 false로 바꿔줌
    }
}

// page를 write buffer 빈공간에 쓰는 함수
void writePageToSpecificBufferSlot(int slot, int logicalBlock, char *buffer) {
    memcpy(writeBuffer[slot].data, buffer, PAGE_SIZE);
    writeBuffer[slot].LPN = logicalBlock;
    writeBuffer[slot].iswritten = true;
    if (writeBuffer[slot].LPN) {
        updateCounter[slot] = 1;
    } else {
        updateCounter[slot] = 0;
    }
}

// write buffer에 있는 data를 수정할때 사용되는 함수
bool tryUpdateExistingBuffer(int logicalBlock, char *buffer) {
    for (int i = 0; i < PAGES_PER_BLOCK; i++) {
        if (writeBuffer[i].iswritten == true && writeBuffer[i].LPN == logicalBlock) {
            writePageToSpecificBufferSlot(i, logicalBlock, buffer);
            return true;
        }
    }
    return false;
}

void writePagetoBuffer(int logicalBlock, char *buffer) {
    // buffer에 해당되는 logicalBlock이 있다면 update
    if (tryUpdateExistingBuffer(logicalBlock, buffer)) {
        return;
    }

    // buffer의 빈공간에 write
    for (; n < PAGES_PER_BLOCK; n++) {
        if (!writeBuffer[n].iswritten) {
            writePageToSpecificBufferSlot(n, logicalBlock, buffer);
            break;
        }
    }

    // buffer가 꽉찼다면 flash에 write
    if (n == PAGES_PER_BLOCK - 1) {
        writePagetoFlash();
        n = 0; // n을 0으로 초기화
    }
}

int main() {
    initFTL();
    initinvalidPagesCounter();

    char buffer[PAGE_SIZE] = "Hello";
    char readBuffer[PAGE_SIZE];
    
    writePagetoBuffer(10, buffer);
    readPage(10, readBuffer);
    printf("Read data: %s\n", readBuffer);
    //printf("Physical block: %d\n", L2P[10].PBN);


    strcpy(buffer, "World");
    writePagetoBuffer(10, buffer);
    readPage(10, readBuffer);
    printf("Read data: %s\n", readBuffer);
    //printf("Physical block: %d\n", L2P[10].PBN);

    return 0;
}
