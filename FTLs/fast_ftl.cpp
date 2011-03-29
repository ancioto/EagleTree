/* Copyright 2011 Matias Bjørling */

/* fast_ftl.cpp  */

/* FlashSim is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version. */

/* FlashSim is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. */

/* You should have received a copy of the GNU General Public License
 * along with FlashSim.  If not, see <http://www.gnu.org/licenses/>. */

/****************************************************************************/

/* Implementation of the FAST FTL described in the paper
 * "A Log buffer-Based Flash Translation Layer Using Fully-Associative Sector Translation by Lee et. al."
 */

#include <new>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include <queue>
#include "../ssd.h"

using namespace ssd;


FtlImpl_Fast::FtlImpl_Fast(Controller &controller):
	FtlParent(controller)
{
	addressShift = 0;
	addressSize = 0;

	// Detect required number of bits for logical address size
	for (int size = SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE * 4; size > 0; addressSize++) size /= 2;

	// Find required number of bits for block size
	for (int size = BLOCK_SIZE/2;size > 0; addressShift++) size /= 2;

	printf("Total required bits for representation: %i (Address: %i Block: %i) \n", addressSize + addressShift, addressSize, addressShift);

	// Trivial assumption checks
	if (sizeof(int) != 4) assert("integer is not 4 bytes");

	// Initialise block mapping table.
	uint numBlocks = SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE;
	data_list = new long[numBlocks];

	for (uint i=0;i<numBlocks;i++)
		data_list[i] = -1;

	sequential_offset = 0;

	printf("Total mapping table size: %luKB\n", numBlocks * sizeof(uint) / 1024);
	printf("Using BAST FTL.\n");
	return;
}

FtlImpl_Fast::~FtlImpl_Fast(void)
{
	delete data_list;

	return;
}

enum status FtlImpl_Fast::read(Event &event)
{
	// Find block
	long lookupBlock = (event.get_logical_address() >> addressShift);
	Address eventAddress;
	eventAddress.set_linear_address(event.get_logical_address());

	LogPageBlock *logBlock = NULL;
	if (log_map.find(lookupBlock) != log_map.end())
		logBlock = log_map[lookupBlock];

	if (data_list[lookupBlock] == -1 && logBlock != NULL && logBlock->pages[eventAddress.page] == -1)
	{
		event.set_address(new Address(0, PAGE));
		fprintf(stderr, "Page read not written. Logical Address: %li\n", event.get_logical_address());
		return FAILURE;
	}

	// If page is in the log block
	if (logBlock != NULL && logBlock->pages[eventAddress.page] != -1)
	{
		Address returnAddress = new Address(logBlock->address.get_linear_address()+logBlock->pages[eventAddress.page], PAGE);
		event.set_address(returnAddress);

		manager.simulate_map_read(event);

		return controller.issue(event);
	} else {
		// If page is in the data block
		Address returnAddress = new Address(data_list[lookupBlock]+ event.get_logical_address() % BLOCK_SIZE , PAGE);
		event.set_address(returnAddress);

		manager.simulate_map_read(event);

		return controller.issue(event);
	}

	return FAILURE;
}


void FtlImpl_Fast::allocate_new_logblock(LogPageBlock *logBlock, long logicalBlockAddress, Event &event)
{
	if (log_map.size() >= PAGE_MAX_LOG)
	{
		long exLogicalBlock = (*log_map.begin()).first;
		LogPageBlock *exLogBlock = (*log_map.begin()).second;

		printf("killing %li with address: %lu\n", exLogicalBlock, exLogBlock->address.get_linear_address());

//		if (!is_sequential(exLogBlock, exLogicalBlock, event))
//			random_merge(exLogBlock, exLogicalBlock, event);


	}

	logBlock = new LogPageBlock();
	logBlock->address = manager.get_free_block(LOG);

	printf("Using new log block with address: %lu Block: %u at logical address: %li\n", logBlock->address.get_linear_address(), logBlock->address.block, logicalBlockAddress);
	log_map[logicalBlockAddress] = logBlock;
}

void FtlImpl_Fast::dispose_logblock(LogPageBlock *logBlock, long logicalBlockAddress)
{
	log_map.erase(logicalBlockAddress);
	delete logBlock;
}

void FtlImpl_Fast::switch_sequential(long logicalBlockAddress, Event &event)
{
	// Add to empty list i.e. switch without erasing the datablock.

	if (data_list[logicalBlockAddress] != -1)
	{
		Address a;
		a.set_linear_address(data_list[logicalBlockAddress], BLOCK);
		manager.invalidate(a, DATA);
	}

	data_list[logicalBlockAddress] = sequential_address.get_linear_address();

	sequential_offset = 0;
	sequential_logical_address = -1;
	sequential_address = manager.get_free_block(DATA);

	manager.simulate_map_write(event);

	printf("Switch sequential\n");
}

void FtlImpl_Fast::merge_sequential(long logicalBlockAddress, Event &event)
{

	if (sequential_logical_address == -1)
		return; // Nothing to merge as it is the first page.

	// Do merge (n reads, n writes and 2 erases (gc'ed))
	Address eventAddress;
	eventAddress.set_linear_address(event.get_logical_address());

	Address newDataBlock = manager.get_free_block(DATA);
	printf("Using new data block with address: %lu Block: %u\n", newDataBlock.get_linear_address(), newDataBlock.block);

	Event *eventOps = event.get_last_event(event);
	Event *newEvent = NULL;
	for (uint i=0;i<BLOCK_SIZE;i++)
	{
		// Lookup page table and see if page exist in log page
		Address readAddress;

		Address seq = Address(sequential_address.get_linear_address() + i, PAGE);
		if (get_state(seq) == VALID)
		{
			readAddress = seq;
		}
		else if (data_list[logicalBlockAddress] != -1)
		{
			readAddress.set_linear_address(data_list[logicalBlockAddress] + i, PAGE);
		}
		else
		{
			printf("Empty page.\n");
			continue;
		}

		if((newEvent = new Event(READ, event.get_logical_address(), 1, event.get_start_time())) == NULL)
		{
			fprintf(stderr, "Ssd error: %s: could not allocate Event\n", __func__);	exit(MEM_ERR);
		}

		newEvent->set_address(readAddress);

		eventOps->set_next(*newEvent);
		eventOps = newEvent;

		if((newEvent = new Event(WRITE, event.get_logical_address(), 1, event.get_start_time())) == NULL)
		{
			fprintf(stderr, "Ssd error: %s: could not allocate Event\n", __func__); exit(MEM_ERR);
		}

		Address dataBlockAddress = new Address(newDataBlock.get_linear_address() + i, PAGE);

		newEvent->set_payload((char*)page_data + readAddress.get_linear_address() * PAGE_SIZE);

		newEvent->set_address(dataBlockAddress);

		eventOps->set_next(*newEvent);
		eventOps = newEvent;
	}

	event.get_last_event(event);

	// Invalidate inactive pages

	Address lBlock = Address(sequential_address);
	manager.invalidate(lBlock, LOG);
	if (data_list[logicalBlockAddress] != -1)
	{
		Address dBlock = new Address(data_list[logicalBlockAddress], BLOCK);
		manager.invalidate(dBlock, DATA);
	}

	// Update mapping
	data_list[logicalBlockAddress] = newDataBlock.get_linear_address();

	// Add erase events if necessary.
	manager.insert_events(event);

	// Add write events if necessary.
	manager.simulate_map_write(event);

	printf("Merge sequential\n");
}

bool FtlImpl_Fast::random_merge(LogPageBlock *logBlock, long logicalBlockAddress, Event &event)
{
	// Do merge (n reads, n writes and 2 erases (gc'ed))
	/* 1. Write page to new data block
	 * 1a Promote new log block.
	 * 2. Create BLOCK_SIZE reads
	 * 3. Create BLOCK_SIZE writes
	 * 4. Invalidate data block
	 * 5. promote new block as data block
	 * 6. put data and log block into the invalidate list.
	 */

	Address eventAddress;
	eventAddress.set_linear_address(event.get_logical_address());

	Address newDataBlock = manager.get_free_block(DATA);
	printf("Using new data block with address: %lu Block: %u\n", newDataBlock.get_linear_address(), newDataBlock.block);

	Event *eventOps = event.get_last_event(event);
	Event *newEvent = NULL;
	for (uint i=0;i<BLOCK_SIZE;i++)
	{
		// Lookup page table and see if page exist in log page
		Address readAddress;
		if (logBlock->pages[eventAddress.page] != -1)
		{
			readAddress.set_linear_address(logBlock->address.real_address + logBlock->pages[i], PAGE);
		}
		else if (data_list[logicalBlockAddress] != -1)
		{
			readAddress.set_linear_address(data_list[logicalBlockAddress] + i, PAGE);
		}
		else
		{
			printf("Empty page.\n");
			continue;
		}

		if((newEvent = new Event(READ, event.get_logical_address(), 1, event.get_start_time())) == NULL)
		{
			fprintf(stderr, "Ssd error: %s: could not allocate Event\n", __func__);	exit(MEM_ERR);
		}

		newEvent->set_address(readAddress);

		eventOps->set_next(*newEvent);
		eventOps = newEvent;

		if((newEvent = new Event(WRITE, event.get_logical_address(), 1, event.get_start_time())) == NULL)
		{
			fprintf(stderr, "Ssd error: %s: could not allocate Event\n", __func__); exit(MEM_ERR);
		}

		Address dataBlockAddress = new Address(newDataBlock.get_linear_address() + i, PAGE);

		newEvent->set_payload((char*)page_data + readAddress.get_linear_address() * PAGE_SIZE);

		newEvent->set_address(dataBlockAddress);

		eventOps->set_next(*newEvent);
		eventOps = newEvent;
	}

	event.get_last_event(event);

	// Invalidate inactive pages
	manager.invalidate(logBlock->address, LOG);
	if (data_list[logicalBlockAddress] != -1)
	{
		Address dBlock = new Address(data_list[logicalBlockAddress], BLOCK);
		manager.invalidate(dBlock, DATA);
	}

	// Update mapping
	data_list[logicalBlockAddress] = newDataBlock.get_linear_address();

	// Add erase events if necessary.
	manager.insert_events(event);

	// Add write events if necessary.
	manager.simulate_map_write(event);

	dispose_logblock(logBlock, logicalBlockAddress);

	return true;
}

bool FtlImpl_Fast::write_to_log_block(Event &event, long logicalBlockAddress)
{
	uint lbnOffset = event.get_logical_address() % BLOCK_SIZE;
	if (lbnOffset == 0) /* Case 1 in Figure 5 */
	{
		if (sequential_offset == BLOCK_SIZE -1)
		{
			switch_sequential(logicalBlockAddress, event);
			/* The log block is filled with sequentially written sectors
			 * Perform switch operation
			 * After switch, the data block is erased and returned to the free-block list
			 */
		} else {
			/* Before merge, a new block is allocated from the free-block list
			 * merge the SW log block with its corresponding data block
			 * after merge, the two blocks are erased and returned to the free-block list
			 */
			merge_sequential(logicalBlockAddress, event);
		}

		/* Get a block from the free-block list and use it as a SW log block
		 * Append data to the SW log block
		 * Update the SW log block part of the sector mapping table
		 */

		sequential_address = manager.get_free_block(DATA);
		sequential_logical_address = logicalBlockAddress;
		sequential_offset = 1;

		Address seq = sequential_address;
		controller.get_free_page(seq);

		event.set_address(seq);


	} else {
		if (sequential_logical_address == logicalBlockAddress) // If the current owner for the SW log block is the same with lbn
		{
			// last_lsn = getLastLsnFromSMT(lbn) Sector mapping table

			if (lbnOffset == sequential_offset)// lsn is equivalent with (last_lsn+1)
			{
				// Append data to the SW log block
				Address seq = sequential_address;
				controller.get_free_page(seq);
				event.set_address(seq);

				sequential_offset++;

			} else {
				// Merge the SW log block with its corresponding data block
				// Get a block from the free-block list and use it as a SW log block
				merge_sequential(logicalBlockAddress, event);

				sequential_offset = 1;
				sequential_address = manager.get_free_block(DATA);
				sequential_logical_address = logicalBlockAddress;
			}

			// Update the SW log block part of the sector mapping table
		} else {
			if (1) // Thereare no rooms in the RW log lock to write data
			{
				/*
				 * Select the first block of the RW log block list as a victim
				 * merge the victim with its corresponding data block
				 * get a block from the free block list and add it to the end of the RW log block list
				 * update the RW log block part of the sector-mapping table
				 *
				 */
			}
			// Append data to the RW log blocks.
		}
	}

	return true;
}



enum status FtlImpl_Fast::write(Event &event)
{
	long logicalBlockAddress = (event.get_logical_address() >> addressShift);
	Address eventAddress; eventAddress.set_linear_address(event.get_logical_address());

	LogPageBlock *logBlock = NULL;

	if (log_map.find(logicalBlockAddress) == log_map.end())
		allocate_new_logblock(logBlock, logicalBlockAddress, event);

	logBlock = log_map[logicalBlockAddress];

	block_state logBlockState = controller.get_block_state(logBlock->address);

	assert(logBlockState != INACTIVE);

	// if a collision occurs at offset of the data block of pbn.

	// else write data at offset in the data block of pbn.

	//

//	// Is it a sequential stream or random
//	if (is_next_sequential(logicalBlockAddress))
//
//
//	// Can it fit inside the existing log block. Issue the request.
//	uint numValid = controller.get_num_valid(&logBlock->address);
//	if (numValid < BLOCK_SIZE)
//	{
//		logBlock->pages[eventAddress.page] = numValid;
//
//		Address logBlockAddress = logBlock->address;
//		controller.get_free_page(logBlockAddress);
//
//		event.set_address(logBlockAddress);
//	} else {
////		if (!is_sequential(logBlock, logicalBlockAddress, event))
////			random_merge(logBlock, logicalBlockAddress, event);
//
//		allocate_new_logblock(logBlock, logicalBlockAddress, event);
//
//		// Write the current io to a new block.
//		logBlock->pages[eventAddress.page] = 0;
//		Address dataPage = logBlock->address;
//		dataPage.valid = PAGE;
//		event.set_address(dataPage);
//	}

	if (controller.issue(event) == FAILURE)
		return FAILURE;

	event.consolidate_metaevent(event);

	return SUCCESS;
}