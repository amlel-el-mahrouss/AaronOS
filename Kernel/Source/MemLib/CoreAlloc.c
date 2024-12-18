#include <GraphicsLib/Terminal.h>
#include <MemLib/CorePaging.h>
#include <MemLib/CoreAlloc.h>
#include <StringUtils.h>
#include <DT/Result.h>

static Boolean gAllocationEnabled = False;

static SizeT	   gMemorySize = 0;
static MemBlk* gBaseAddress = NULL;
static MemBlk* gHighestAddress = NULL;

Boolean 
MemEnabled(void)
{
	return gAllocationEnabled;
}

VoidPtr 
MemStart(Void)
{
	return gBaseAddress;
}

VoidPtr 
MemEnd(Void)
{
	return gHighestAddress;
}

Boolean 
MemInit(BootloaderHeader* bootHeader) 
{
    Check(!gAllocationEnabled, "Mem API is already enabled!");

	struct TagMemmap* entries = BootloaderTag(bootHeader, EKBOOT_STRUCT_TAG_MEM_ID);

	for (SizeT i = 0; i < entries->entries; i++)
	{
		if (entries->memmap[i].type == EKBOOT_MEM_USABLE)
		{
			if (IsNull(gBaseAddress))
			{
				gBaseAddress = (VoidPtr)(entries->memmap[i].base);
				gBaseAddress->Next = NULL;
				continue;
			} else if (IsNull(gBaseAddress))
			{
				gHighestAddress = (VoidPtr)(entries->memmap[i].base);

				gHighestAddress->Next = NULL;
				continue;
			}

			break;
		}
	}

	gMemorySize = ( -((UIntPtr)&gBaseAddress - (UIntPtr)&gHighestAddress)) * sizeof(struct MemBlk);

	gHighestAddress->Prev = gBaseAddress;
    gHighestAddress->Next = gBaseAddress;

	gBaseAddress->Prev = gHighestAddress;

    gAllocationEnabled = True;

	for (SizeT i = 0; i < MEM_MAX_HEADERS; ++i){
		gBaseAddress->Index[i].Used = False;
		gHighestAddress->Index[i].Used = False;
		gBaseAddress->Index[i].Magic = MEM_MAGIC;
		gHighestAddress->Index[i].Magic = MEM_MAGIC;
	}

    ConsoleLog("Memory Heap is enabled.\n");
	ConsoleLog("%s %x %n", "Memory Size: ", gMemorySize);
	ConsoleLog("%s %x %s %x %n", "From: ", (UIntPtr)&gBaseAddress, " To: ", (UIntPtr)&gHighestAddress);

    return True;
}

// Allocation heer struct
// Also Used to cache recent allocated addresses

typedef struct AllocLock {
	Boolean bLocked;
	SizeT lastLockedSize;
	UIntPtr lastLockedAddr;
} Attribute((packed)) AllocLock;

static struct AllocLock gAllocLock = {
		.lastLockedAddr = 0,
		.lastLockedSize = 0,
		.bLocked = False,
};

// Initializes an allocation list
static 
Void 
MemInitAllocationList(MemBlk* block) {
	Check(NotNull(block), "Block is NULL! (MemInitAllocationList)");

	if (block->Index[0].Magic != MEM_MAGIC) {
		for (SizeT i = 0; i < MEM_MAX_HEADERS; ++i){
			Check(!block->Index[i].Used, "Critical Issue detected in the allocator! Entry is Used, while being set the Magic bit.");
			block->Index[i].Magic = MEM_MAGIC;
		}
	}
}

/*
 *  Reserves a VirtualAddress according an offset.
 *  Where offset == block -> true
 *  Returns null if no free headers were found.
 *  Also returns null if the Index pointer is not valid.
 */

static
VoidPtr 
MemReserveBlock(MemBlk* block, SizeT index, SizeT Size) {
	if (gAllocLock.bLocked) return NULL;
	if (block->Index[index].Size > (KIB * 4)) { block->Index[index].Used = True; Result = ERR_BAD_ACCESS; return NULL; }
	if (((Size + 4096) & 16) == 16) Size += 16;

	gAllocLock.bLocked = True;

	block->Index[index].VirtualAddress = (VoidPtr)(block + Size);
	block->Index[index].Magic = MEM_MAGIC;
	block->Index[index].Size += Size;
	block->Index[index].Used = True;

	gAllocLock.lastLockedAddr = (UIntPtr)(block->Index[index].VirtualAddress);
	gAllocLock.lastLockedSize = Size;

	gAllocLock.bLocked = False;

	Result = ERR_SUCCESS;

	return block->Index[index].VirtualAddress;
}

static
VoidPtr 
MemAllocBlock(MemBlk* block, SizeT Size) {
    Result = MEM_NOT_ENABLED;
    if (!gAllocationEnabled) return NULL;

    Result = ERR_NULL;
    if (block == NULL) return NULL;

	SizeT index = 0;

	SizeT indexLeft = 0;
	SizeT indexRight = MEM_MAX_HEADERS;

	while (indexLeft < indexRight) {
			VoidPtr result = MemReserveBlock(block, index, Size);

			if (result != NULL) {
				Result = ERR_SUCCESS;
				return result;
			} else {
				if (index == indexLeft) {
					++indexLeft;
					index = indexRight;
				} else {
					--indexRight;
					index = indexLeft;
				}
			}
	}

    Result = MEM_UNAVAILABLE; // we are out of free allocation table
    return NULL;
}

static
MemBlk*
 MemAllocNextBlock(MemBlk* block) {
    if (block == NULL) {
		ConsoleLog("!!!!OUT OF MEMORY!!!!");

        Result = MEM_OUT_OF_MEMORY;
        return False;
    } // We are out of memory

    if (block->Next != NULL) {
        Result = ERR_NOT_NULL;
        return False;
    } // Skip this block

    // Or if empty, fill this block
    block->Next = block + sizeof(MemBlk);
    block->Next->Prev = block;

    block = block->Next;

    Result = ERR_SUCCESS;
    return block;
}

static
Void 
MemExpandBlock(MemBlk* Current) {
	if ((Current = MemAllocNextBlock(Current)) == NULL) {
		Int32 tmp = Result; // stock the Result

		switch (tmp) { // Fetch why it failed.
			case (MEM_OUT_OF_MEMORY): {
				ConsoleLog("Out Of Memory!");
				Asm ("int $25");
				break;
			} default:
				Current = Current->Next;
				break;

		}
	}
}

VoidPtr 
MemAlloc(SizeT Size) {
    Result = ERR_FAILURE;
    if (!gAllocationEnabled) return NULL;

	Result = MEM_INVALID_SIZE;
    if (Size < 1) return NULL;

	VoidPtr allocatedBlock = NULL;
	MemBlk* currentBlock = gBaseAddress;
	MemInitAllocationList(currentBlock);

	// seek and obtain strategy.
	while (allocatedBlock == NULL) {
		for (SizeT i = 0; i < MEM_MAX_HEADERS; ++i) {
			Check(currentBlock->Index[i].Magic == MEM_MAGIC, "Possible memory corruption detected in the kernel heap! Aborting system...");
		}
		
		allocatedBlock = MemAllocBlock(currentBlock, Size);
		if (Result == MEM_UNAVAILABLE) MemExpandBlock(allocatedBlock);
	}

	// ConsoleLog(Ternary(NotNull(currentBlock), "%s, %x, %n ", "%s, %n") , Ternary(NotNull(currentBlock), "Allocated Block at: ", (VoidPtr)&currentBlock));
	return allocatedBlock;
}

VoidPtr MemResize(VoidPtr Pointer, SizeT iSize) {
	Result = MEM_BAD_ARG;

	if (!gAllocationEnabled) return NULL;
	if (Pointer == NULL) return NULL;
	if (iSize == 0) return NULL;

	MemBlk* currentBlock = gBaseAddress;

	while (currentBlock != NULL) {
		for (SizeT index = 0; index < MEM_MAX_HEADERS; ++index) {
			if (currentBlock->Index[index].VirtualAddress != Pointer) continue;

			char* values = Pointer;
			char barrier = values[iSize];

			SizeT count = iSize;

			while (count) {
				if (values[count] != barrier)
					values[count] = 0;
				else
					break; // Return null, don't touch that

				--count;
			}

			currentBlock->Index[index].VirtualAddress = (VoidPtr)values;
			currentBlock->Index[index].Size += iSize;

			Result = ERR_SUCCESS;
			return currentBlock->Index[index].VirtualAddress;
		}

		currentBlock = currentBlock->Next;
	}

	Result = MEM_NOT_ENABLED;
	return NULL;
}

static
Int32 
MemFreeBlock(MemBlk* currentBlock, SizeT index, VoidPtr ptr) {
	if (!currentBlock->Index[index].Used) return -1;
	if (currentBlock->Index[index].VirtualAddress != ptr) return -1;

	SetMem(ptr, 0, StringLength(ptr));
	currentBlock->Index[index].Size -= StringLength(ptr);

	if (currentBlock->Index[index].Size < 1) currentBlock->Index[index].Used = False; // we're not using it anymore.

	return 0;
}

Int32
MemFree(VoidPtr Alloc) {
    Result = ERR_FAILURE;
    if (!gAllocationEnabled) return MEM_NOT_ENABLED;
	
	Result = ERR_NULL;
	if (Alloc == NULL) return MEM_BAD_ARG;

    MemBlk* currentBlock = gBaseAddress;

    while (currentBlock != NULL) {
        for (SizeT index = 0; index < MEM_MAX_HEADERS; ++index) {
           	if (MemFreeBlock(currentBlock, index, Alloc) == -1) continue;

			Result = ERR_SUCCESS;
			Alloc = NULL; // Alloc points now to an invalid VirtualAddress.
            return ERR_SUCCESS;
        }

        currentBlock = currentBlock->Next;
    }

    Result = MEM_UNAVAILABLE;
    return MEM_UNAVAILABLE; // Fatal! Check Result when -2 is being pushed on the stack.
}
