#include <stdio.h>
#include <string.h>
#include <stdbool.h>


#define TOTAL_BLOCKS 256
#define PAGES_PER_BLOCK 16
#define PAGE_SIZE 4096

// 现有问题是两种GC, 一种是空闲页合并，一种是新旧数据合并，
// 但空闲页合并会导致块内page位置改变，为了可以追中页， 或许需要将映射表改为页级别的映射表（逻辑块对应一个物理页）
// （由于代码中的block结构内存在page，并且read/write函数中也接收page参数，所以实际上已经实现了页级映射）

typedef struct {
    char data[PAGE_SIZE];
} Page;

typedef struct {
    Page pages[PAGES_PER_BLOCK];
    int valid[PAGES_PER_BLOCK]; 
} Block;

Block flash[TOTAL_BLOCKS];
Block GCBuffer;

int L2P[TOTAL_BLOCKS][PAGES_PER_BLOCK]; //或许我需要将它修改为二维数组， 以实现页级映射
int P2L[TOTAL_BLOCKS]; 

void initFTL() { 
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        P2L[i] = -1;
        for (int j = 0; j < PAGES_PER_BLOCK; j++) {
            L2P[i][j] = -1;
            flash[i].valid[j] = 0;
        }
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

void eraseBlock(int physicalBlock) {
    if (physicalBlock != -1) {
        int logicalBlock = P2L[physicalBlock];
        for (int j = 0; j < PAGES_PER_BLOCK; j++) {
            memset(flash[physicalBlock].pages[j].data, 0, PAGE_SIZE);
            flash[physicalBlock].valid[j] = 0;
            L2P[logicalBlock][j] = -1;
        }
        P2L[physicalBlock] = -1;
    }
}

// 打算设计两个GC策略， 空闲页合并（用于worst case， 已实现）和新旧数据合并（已完成）
// void garbageCollector() {
//     int targetBlock = -1;
//     int maxInvalidPages = -1;

//     for (int i = 0; i < TOTAL_BLOCKS; i++) {
//         int currentInvalidPages = invalidPagesInBlock(i);
//         if (currentInvalidPages > maxInvalidPages) {
//             targetBlock = i;
//             maxInvalidPages = currentInvalidPages;
//         }
//     }

//     if (targetBlock != -1) {
//         memcpy(&GCBuffer, &flash[targetBlock], sizeof(Block)); // 将目标块的数据拷贝到GCBuffer中
//         eraseBlock(P2L[targetBlock]);
//     }
// }

// 空闲页合并，（暂且只支持最大两个块的合并）
void garbageCollector_forTheWorst() {
    int HtargetBlock = -1;
    int LtargetBlock = -1;
    int MaxInvalidPages = -1;
    int RemainingPages = 0;

    for (int i = 0; i < TOTAL_BLOCKS; i++){
        int currentInvalidPages = invalidPagesInBlock(i);
        if (currentInvalidPages > MaxInvalidPages){
            HtargetBlock = i;
            MaxInvalidPages = currentInvalidPages;
            RemainingPages = PAGES_PER_BLOCK - MaxInvalidPages;
        }
    }
    bool foundLtargetBlock = false;
    for (int j = 0; j < TOTAL_BLOCKS && !foundLtargetBlock; j++){
        int currentInvalidPages = invalidPagesInBlock(j);
        if (currentInvalidPages == RemainingPages || (currentInvalidPages < RemainingPages && currentInvalidPages > 0)){
            LtargetBlock = j;
            foundLtargetBlock = true;
        }
        else if (currentInvalidPages < RemainingPages && currentInvalidPages > 0){
            LtargetBlock = j;
        }
    }

    if (HtargetBlock != -1 && LtargetBlock != -1){
        int l = 0;
        for (int k = 0; k < PAGES_PER_BLOCK; k++){
            if (flash[HtargetBlock].valid[k]){
                memcpy(GCBuffer.pages[l].data, flash[HtargetBlock].pages[k].data, PAGE_SIZE);
                l++;
            }
        }
        int L = l;
        for (int k = 0; k < PAGES_PER_BLOCK; k++){
            if (flash[LtargetBlock].valid[k]){
                memcpy(GCBuffer.pages[l].data, flash[LtargetBlock].pages[k].data, PAGE_SIZE);
                l++;
            }
        }
        eraseBlock(P2L[HtargetBlock]);
        eraseBlock(P2L[LtargetBlock]);
        int GCnewPhysicalBlock = allocateBlock();
        for (int k = 0; k < L; k++){
            memcpy(flash[GCnewPhysicalBlock].pages[k].data, GCBuffer.pages[k].data, PAGE_SIZE);
            flash[GCnewPhysicalBlock].valid[k] = 1;
            L2P[P2L[HtargetBlock]][k] = GCnewPhysicalBlock;
        }
        for (int k = L; k < PAGES_PER_BLOCK; k++){
            memcpy(flash[GCnewPhysicalBlock].pages[k].data, GCBuffer.pages[k].data, PAGE_SIZE);
            flash[GCnewPhysicalBlock].valid[k] = 1;
            L2P[P2L[LtargetBlock]][k] = GCnewPhysicalBlock;
        }
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
    } else {
        int newPhysicalBlock = allocateBlock();
        if (newPhysicalBlock == -1) {
            garbageCollector_forTheWorst();
            newPhysicalBlock = allocateBlock();
        }
        memcpy(GCBuffer.pages[page].data, buffer, PAGE_SIZE);
        GCBuffer.valid[page] = 1;
        for (int j = 0; j < PAGES_PER_BLOCK; j++) {
            if (flash[physicalBlock].valid[j]) {
                memcpy(flash[newPhysicalBlock].pages[j].data, GCBuffer.pages[j].data, PAGE_SIZE);
                flash[newPhysicalBlock].valid[j] = 1;
            }
        }
        eraseBlock(logicalBlock);
        L2P[logicalBlock][page] = newPhysicalBlock;
        P2L[newPhysicalBlock] = logicalBlock;
    }
}

int main() {
    initFTL();

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
