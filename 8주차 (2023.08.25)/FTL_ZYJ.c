#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define TOTAL_BLOCKS 256
#define BLOCK_SIZE 16
#define PAGE_SIZE 4096
///////////////////////
#define GCTrigger 0.6//
#define ECC 50       //
///////////////////////
typedef struct {
    char data[PAGE_SIZE];
} Page;

typedef struct {
    Page pages[BLOCK_SIZE];
    int valid[BLOCK_SIZE];
} Block;

typedef struct {
    int PBN;
    int PPN;
} PhysicalLocation; 

typedef struct {
    int PPN[BLOCK_SIZE];
} LogicalLocation;


typedef struct {
    char data[PAGE_SIZE];
    int LPN;
    bool iswritten;
} PWBuffer;

Block flash[TOTAL_BLOCKS];
Block GCBuffer; // GC를 위한 buffer
PWBuffer writeBuffer[BLOCK_SIZE]; // write buffer


PhysicalLocation L2P[TOTAL_BLOCKS * BLOCK_SIZE]; // page mapping table
LogicalLocation P2L[TOTAL_BLOCKS]; // physical block mapping table, 1차배열을 struct배열로 변경, 즉 page들을 block마다 격리

int invalidPagesCounter[TOTAL_BLOCKS]; // block 내의 invalid page 수를 저장하는 배열
int GCMappingSupporter[BLOCK_SIZE]; 
int GCBufferLogicalMapping[BLOCK_SIZE * 2]; 
int GCBufferPageCount[TOTAL_BLOCKS];
int updateCounter[BLOCK_SIZE];
int n = 0; // write buffer에 들어있는 page 수를 세는 변수

// FTL초기화
void initFTL() {
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        for (int j = 0; j < BLOCK_SIZE; j++) {
            flash[i].valid[j] = 0;
            P2L[i].PPN[j] = -1;  
        }
        invalidPagesCounter[i] = BLOCK_SIZE;
    }
    for (int i = 0; i < TOTAL_BLOCKS * BLOCK_SIZE; i++) {
        L2P[i].PBN = -1;
        L2P[i].PPN = -1;
    }
    for (int i = 0; i < BLOCK_SIZE * 2; i++) {
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
        if (invalidPagesCounter[i] == BLOCK_SIZE) {
            return i;
        }
    }
    return -1;
}

// Block 내의 invalid page 수를 계산하는 함수
int invalidPagesInBlock(int block) { 
    int count = 0;
    for (int j = 0; j < BLOCK_SIZE; j++) {
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
        for (int j = 0; j < BLOCK_SIZE; j++) {
            int logicalPage = P2L[physicalBlock].PPN[j]; 
            memset(flash[physicalBlock].pages[j].data, 0, PAGE_SIZE);
            flash[physicalBlock].valid[j] = 0;
            if(logicalPage != -1) {
                L2P[logicalPage].PBN = -1;
                L2P[logicalPage].PPN = -1;
            }
            P2L[physicalBlock].PPN[j] = -1; 
        }
        invalidPagesCounter[physicalBlock] = BLOCK_SIZE;
    }
}

// Page merge GC (only 2 blocks supported)
void moveValidPagesToBuffer(int targetBlock, int *l) {
    for (int k = 0; k < BLOCK_SIZE; k++) {
        if (flash[targetBlock].valid[k] && L2P[P2L[targetBlock].PPN[k]].PBN == targetBlock) {
            memcpy(GCBuffer.pages[*l].data, flash[targetBlock].pages[k].data, PAGE_SIZE);
            GCMappingSupporter[*l] = k;
            GCBufferLogicalMapping[*l] = P2L[targetBlock].PPN[k];
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
        if (currentInvalidPages > GCTrigger * BLOCK_SIZE) {
            HtargetBlock = i;
            MaxInvalidPages = currentInvalidPages;
            RemainingPages = BLOCK_SIZE - MaxInvalidPages;
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
            P2L[GCnewPhysicalBlock].PPN[k] = logicalPage; 
        }  
        invalidPagesCounter[GCnewPhysicalBlock] = BLOCK_SIZE - l;
    }
}

void readPage(int logicalBlock, char *buffer) {
    // check write buffer first
    for (int i = 0; i < BLOCK_SIZE; i++) {
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
    for (int i = 0; i < BLOCK_SIZE; i++) {
        memcpy(flash[newPhysicalBlock].pages[i].data, writeBuffer[i].data, PAGE_SIZE);
        flash[newPhysicalBlock].valid[i] = 1;
        L2P[writeBuffer[i].LPN].PBN = newPhysicalBlock;
        L2P[writeBuffer[i].LPN].PPN = i;
        P2L[newPhysicalBlock].PPN[i] = writeBuffer[i].LPN; // 使用新的P2L结构
        if (updateCounter[i]) {
            invalidPagesCounter[newPhysicalBlock]++;
        }
        writeBuffer[i].iswritten = false;
    }
}

// page를 write buffer 빈공간에 쓰는 함수
void writePageToSpecificBufferSlot(int slot, int logicalBlock, char *buffer) {
    memcpy(writeBuffer[slot].data, buffer, PAGE_SIZE - ECC);
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
    for (int i = 0; i < BLOCK_SIZE; i++) {
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
    for (; n < BLOCK_SIZE; n++) {
        if (!writeBuffer[n].iswritten) {
            writePageToSpecificBufferSlot(n, logicalBlock, buffer);
            break;
        }
    }

    // buffer가 꽉찼다면 flash에 write
    if (n == BLOCK_SIZE - 1) {
        writePagetoFlash();
        n = 0; // n을 0으로 초기화
    }
}

int main() {
    initFTL();
    initinvalidPagesCounter();

    char buffer[PAGE_SIZE - ECC] = "Hello";
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
