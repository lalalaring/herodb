#include "Log.h"

/*
 *	Page Structure
 *		Index Page				: [uint64 AddressItems][uint64 NextIndexPage]{[uint64 AddressItem] ...}
 *			The first index page is the index page from the source.
 *			Each address item is the address of transactions of 0, 1, ...
 * 		Log Page				:
 * 			Transaction Header	: [uint64 Transaction]<Item-Header>
 * 				Min Size = 4 * sizeof(uint64)
 * 			Item Header			: [uint64 ItemLength]<Item-Block>
 * 				Min Size = 3 * sizeof(uint64)
 * 			Item Block			: [uint64 CurrentBlockLength][uint64 NextBlockAddress/NextItemAddress][data, adjust to sizeof(uint64)]
 * 				Min Size = 2 * sizeof(uint64)
 */

#define INDEX_INVALID (~(vuint64_t)0)

#define INDEX_INDEXPAGE_ADDRESSITEMS 0
#define INDEX_INDEXPAGE_NEXTINDEXPAGE 1
#define INDEX_INDEXPAGE_ADDRESSITEMBEGIN 2

namespace vl
{
	namespace database
	{
/***********************************************************************
LogManager::LogWriter
***********************************************************************/

		LogManager::LogWriter::LogWriter(LogManager* _log, BufferTransaction _trans)
			:log(_log)
			,trans(_trans)
			,opening(true)
		{
		}

		LogManager::LogWriter::~LogWriter()
		{
			if (opening)
			{
				Close();
			}
		}

		BufferTransaction LogManager::LogWriter::GetTransaction()
		{
			return trans;
		}

		stream::IStream& LogManager::LogWriter::GetStream()
		{
			return stream;
		}

		bool LogManager::LogWriter::IsOpening()
		{
			return opening;
		}

		bool LogManager::LogWriter::Close()
		{
			if (!opening) return false;
			SPIN_LOCK(log->lock)
			{
				auto desc = log->activeTransactions[trans];
				vint numberCount = desc->firstItem.IsValid() ? 3 : 4;

				vuint64_t written = 0;
				vuint64_t remain = stream.Size();
				stream.SeekFromBegin(0);
				while (true)
				{
					vuint64_t itemHeader = numberCount * sizeof(vuint64_t);
					vuint64_t blockSize = itemHeader + remain;
					BufferPointer address;
					CHECK_ERROR(log->AllocateBlock(itemHeader, blockSize, address), L"vl::database::LogManager::LogWriter::Close()#Internal error: Unable to allocate blocks for saving logs.");
					vuint64_t dataSize = blockSize - itemHeader;

					BufferPage page;
					vuint64_t offset;
					CHECK_ERROR(log->bm->DecodePointer(address, page, offset), L"vl::database::LogManager::LogWriter::Close()#Internal error: Unable to decode block address for saving logs.");
					auto pointer = (char*)log->bm->LockPage(log->source, page);
					auto numbers = (vuint64_t*)(pointer + offset);

					switch (numberCount)
					{
						case 4:
							*numbers++ = trans.index;
						case 3:
							*numbers++ = stream.Size();
						case 2:
							*numbers++ = dataSize;
							*numbers ++ = INDEX_INVALID;
					}
					stream.Read(numbers, (remain < dataSize ? remain : dataSize));
					CHECK_ERROR(log->bm->UnlockPage(log->source, page, pointer, PersistanceType::ChangedAndPersist), L"vl::database::LogManager::LogWriter::Close()#Internal error: Unable to unlock page for saving logs.");
					
					if (numberCount == 4)
					{
						desc->firstItem = address;
						CHECK_ERROR(log->WriteAddressItem(trans, address), L"vl::database::LogManager::LogWriter::Close()#Internal error: Unable to save logs.");
					}
					else if (desc->lastItem.IsValid())
					{
						BufferPage lastItemPage;
						vuint64_t lastItemOffset;
						CHECK_ERROR(log->bm->DecodePointer(desc->lastItem, lastItemPage, lastItemOffset), L"vl::database::LogManager::LogWriter::Close()#Internal error: Unable to decode block address for saving logs.");

						auto pointer = log->bm->LockPage(log->source, lastItemPage);
						*(vuint64_t*)((char*)pointer + lastItemOffset) = address.index;
						CHECK_ERROR(log->bm->UnlockPage(log->source, lastItemPage, pointer, PersistanceType::ChangedAndPersist), L"vl::database::LogManager::LogWriter::Close()#Internal error: Unable to save logs.");
					}
					CHECK_ERROR(log->bm->EncodePointer(desc->lastItem, page, offset + (numberCount - 1) * sizeof(vuint64_t)), L"vl::database::LogManager::LogWriter::Close()#Internal error: Unable to encode block address for saving logs.");

					if (remain > dataSize)
					{
						written += dataSize;
						remain -= dataSize;
						numberCount = 2;
					}
					else
					{
						break;
					}
				}

				opening = false;
				desc->writer = 0;
			}
			return true;
		}

/***********************************************************************
LogManager::LogReader
***********************************************************************/

		LogManager::LogReader::LogReader(LogManager* _log, BufferTransaction _trans)
			:log(_log)
			,trans(_trans)
			,item(BufferPointer::Invalid())
		{
			vint index = log->activeTransactions.Keys().IndexOf(trans);
			if (index == -1)
			{
				item = log->ReadAddressItem(trans);
			}
			else
			{
				auto desc = log->activeTransactions[trans];
				item = desc->firstItem;
			}

			if (item.IsValid())
			{
				BufferPage page;
				vuint64_t offset;
				CHECK_ERROR(log->bm->DecodePointer(item, page, offset), L"vl::database::LogManager::LogReader::LogReader(LogManager*, BufferTransaction)#Internal error: Unable to decode block pointer for reading logs.");
				
				offset += sizeof(vuint64_t);
				CHECK_ERROR(log->bm->EncodePointer(item, page, offset), L"vl::database::LogManager::LogReader::LogReader(LogManager*, BufferTransaction)#Internal error: Unable to decode block pointer for reading logs.");
			}
		}

		BufferTransaction LogManager::LogReader::GetTransaction()
		{
			return trans;
		}

		stream::IStream& LogManager::LogReader::GetStream()
		{
			return *stream.Obj();
		}

		LogManager::LogReader::~LogReader()
		{
		}

		bool LogManager::LogReader::NextItem()
		{
			if (!item.IsValid()) return false;
			stream = new stream::MemoryStream();

			BufferPage page;
			vuint64_t offset;
			CHECK_ERROR(log->bm->DecodePointer(item, page, offset), L"vl::database::LogManager::LogReader::NextItem()#Internal error: Unable to decode block pointer.");
			auto pointer = log->bm->LockPage(log->source, page);
			auto numbers = (vuint64_t*)((char*)pointer + offset);
			auto remain = numbers[0];
			auto block = numbers + 1;

			while (true)
			{
				auto blockSize = block[0];
				item.index = block[1];
				if (blockSize > remain)
				{
					blockSize = remain;
				}
				
				if (blockSize > 0)
				{
					stream->Write(block + 2, blockSize);
				}

				remain -= blockSize;
				CHECK_ERROR(log->bm->UnlockPage(log->source, page, pointer, PersistanceType::NoChanging), L"vl::database::LogManager::LogReader::NextItem()#Internal error: Unable to unlock page.");

				if (remain == 0 || !item.IsValid())
				{
					break;
				}
				CHECK_ERROR(log->bm->DecodePointer(item, page, offset), L"vl::database::LogManager::LogReader::NextItem()#Internal error: Unable to decode pointer.");
				pointer = log->bm->LockPage(log->source, page);
				numbers = (vuint64_t*)((char*)pointer + offset);
				block = numbers;
			}
			stream->SeekFromBegin(0);

			return true;
		}

/***********************************************************************
LogManager
***********************************************************************/

		BufferPointer LogManager::ReadAddressItem(BufferTransaction transaction)
		{
			if (transaction.index >= usedTransactionCount)
			{
				return BufferPointer::Invalid();
			}

			vuint64_t index = transaction.index / indexPageItemCount;
			vuint64_t item = transaction.index % indexPageItemCount;
			if (index >= indexPages.Count())
			{
				return BufferPointer::Invalid();
			}

			BufferPage page = indexPages[index];
			auto numbers = (vuint64_t*)bm->LockPage(source, page);
			if (!numbers) return BufferPointer::Invalid();
			auto result = numbers[item + INDEX_INDEXPAGE_ADDRESSITEMBEGIN];
			bm->UnlockPage(source, page, numbers, PersistanceType::ChangedAndPersist);

			BufferPointer address{result};
			return address;
		}

		bool LogManager::WriteAddressItem(BufferTransaction transaction, BufferPointer address)
		{
			if (transaction.index >= usedTransactionCount)
			{
				return false;
			}

			vuint64_t index = transaction.index / indexPageItemCount;
			vuint64_t item = transaction.index % indexPageItemCount;
			if (index > indexPages.Count())
			{
				return false;
			}

			if (index < indexPages.Count())
			{
				BufferPage page = indexPages[index];
				auto numbers = (vuint64_t*)bm->LockPage(source, page);
				if (!numbers) return false;
				auto& count = numbers[INDEX_INDEXPAGE_ADDRESSITEMS];
				if (count <= item)
				{
					count = item + 1;
				}
				numbers[item + INDEX_INDEXPAGE_ADDRESSITEMBEGIN] = address.index;
				bm->UnlockPage(source, page, numbers, PersistanceType::ChangedAndPersist);
			}
			else
			{
				BufferPage lastPage = indexPages[indexPages.Count() - 1];
				BufferPage currentPage = bm->AllocatePage(source);
				if (!currentPage.IsValid())
				{
					return false;
				}

				auto numbers = (vuint64_t*)bm->LockPage(source, lastPage);
				if (!numbers) return false;
				numbers[INDEX_INDEXPAGE_NEXTINDEXPAGE] = currentPage.index;
				bm->UnlockPage(source, lastPage, numbers, PersistanceType::ChangedAndPersist);

				numbers = (vuint64_t*)bm->LockPage(source, currentPage);
				memset(numbers, 0, pageSize);
				numbers[INDEX_INDEXPAGE_ADDRESSITEMS] = 1;
				numbers[INDEX_INDEXPAGE_NEXTINDEXPAGE] = INDEX_INVALID;
				numbers[item + INDEX_INDEXPAGE_ADDRESSITEMBEGIN] = address.index;
				bm->UnlockPage(source, currentPage, numbers, PersistanceType::ChangedAndPersist);
				indexPages.Add(currentPage);
			}

			return true;
		}

		bool LogManager::AllocateBlock(vuint64_t minSize, vuint64_t& size, BufferPointer& address)
		{
			if (minSize == 0 || size == 0 || size < minSize) return false;
			minSize = IntUpperBound(minSize, sizeof(vuint64_t));
			if (minSize > pageSize) return false;
			size = IntUpperBound(size, sizeof(vuint64_t));

			if (!nextBlockAddress.IsValid())
			{
				BufferPage page = bm->AllocatePage(source);
				if (!page.IsValid()) return false;
				if (!bm->EncodePointer(nextBlockAddress, page, 0)) return false;
			}

			BufferPage page;
			vuint64_t offset;
			if (!bm->DecodePointer(nextBlockAddress, page, offset)) return false;

			vuint64_t remain = pageSize - offset;
			if (remain < minSize)
			{
				nextBlockAddress = BufferPointer::Invalid();
				return AllocateBlock(minSize, size, address);
			}
			else
			{
				if (size > remain)
				{
					size = remain;
				}
				address = nextBlockAddress;
				if (offset + size >= pageSize)
				{
					nextBlockAddress = BufferPointer::Invalid();
				}
				else
				{
					bm->EncodePointer(nextBlockAddress, page, offset + size);
				}
			}

			return true;
		}

		LogManager::LogManager(BufferManager* _bm, BufferSource _source, bool _createNew, bool _autoUnload)
			:bm(_bm)
			,source(_source)
			,autoUnload(_autoUnload)
			,pageSize(0)
			,indexPageItemCount(0)
			,usedTransactionCount(0)
			,nextBlockAddress(BufferPointer::Invalid())
		{
			pageSize = bm->GetPageSize();
			indexPageItemCount = (pageSize - INDEX_INDEXPAGE_ADDRESSITEMBEGIN * sizeof(vuint64_t)) / sizeof(vuint64_t);

			if (_createNew)
			{
				BufferPage page = bm->GetIndexPage(source);
				indexPages.Add(page);

				auto numbers = (vuint64_t*)bm->LockPage(source, page);
				memset(numbers, 0, pageSize);
				numbers[INDEX_INDEXPAGE_ADDRESSITEMS] = 0;
				numbers[INDEX_INDEXPAGE_NEXTINDEXPAGE] = INDEX_INVALID;
				bm->UnlockPage(source, page, numbers, PersistanceType::ChangedAndPersist);
			}
			else
			{
				BufferPage page = bm->GetIndexPage(source);
				BufferPage previousPage = BufferPage::Invalid();
				while (page.IsValid())
				{
					previousPage = page;
					indexPages.Add(page);
					auto numbers = (vuint64_t*)bm->LockPage(source, previousPage);
					page.index = numbers[INDEX_INDEXPAGE_NEXTINDEXPAGE];
					usedTransactionCount += numbers[INDEX_INDEXPAGE_ADDRESSITEMS];
					bm->UnlockPage(source, previousPage, numbers, PersistanceType::NoChanging);
				}
			}
		}

		LogManager::~LogManager()
		{
			if (autoUnload)
			{
				bm->UnloadSource(source);
			}
		}

		vuint64_t LogManager::GetUsedTransactionCount()
		{
			return usedTransactionCount;
		}

		BufferTransaction LogManager::GetTransaction(vuint64_t index)
		{
			if (index < usedTransactionCount)
			{
				BufferTransaction trans{index};
				return trans;
			}
			else
			{
				return BufferTransaction::Invalid();
			}
		}

		BufferTransaction LogManager::OpenTransaction()
		{
			BufferTransaction trans;
			SPIN_LOCK(lock)
			{
				trans.index = INCRC(&usedTransactionCount) - 1;
				BufferPointer address = BufferPointer::Invalid();
				WriteAddressItem(trans, address);

				auto desc = MakePtr<LogTransDesc>();
				desc->firstItem = BufferPointer::Invalid();
				desc->lastItem = BufferPointer::Invalid();

				activeTransactions.Add(trans, desc);
			}
			return trans;
		}

		bool LogManager::CloseTransaction(BufferTransaction transaction)
		{
			SPIN_LOCK(lock)
			{
				auto index = activeTransactions.Keys().IndexOf(transaction);
				if (index == -1) return false;

				auto desc = activeTransactions.Values()[index];
				if (desc->writer && desc->writer->IsOpening()) return false;

				activeTransactions.Remove(transaction);
			}
			return true;
		}

		bool LogManager::IsActive(BufferTransaction transaction)
		{
			SPIN_LOCK(lock)
			{
				return activeTransactions.Keys().Contains(transaction);
			}
			CHECK_ERROR(false, L"vl::database::LogManager::IsActive(BufferTransaction)#Internal error: Should not reach here.");
		}

		Ptr<ILogWriter> LogManager::OpenLogItem(BufferTransaction transaction)
		{
			SPIN_LOCK(lock)
			{
				auto index = activeTransactions.Keys().IndexOf(transaction);
				if (index == -1) return nullptr;

				auto desc = activeTransactions.Values()[index];
				if (desc->writer) return nullptr;

				desc->writer = new LogWriter(this, transaction);
				return desc->writer;
			}
			CHECK_ERROR(false, L"vl::database::LogManager::OpenLogItem(BufferTransaction)#Internal error: Should not reach here.");
		}

		Ptr<ILogReader> LogManager::EnumLogItem(BufferTransaction transaction)
		{
			SPIN_LOCK(lock)
			{
				if (activeTransactions.Keys().Contains(transaction))
				{
					return new LogReader(this, transaction);
				}
			}
			return nullptr;
		}

		Ptr<ILogReader> LogManager::EnumInactiveLogItem(BufferTransaction transaction)
		{
			SPIN_LOCK(lock)
			{
				if (transaction.index < usedTransactionCount && !activeTransactions.Keys().Contains(transaction))
				{
					return new LogReader(this, transaction);
				}
			}
			return nullptr;
		}
	}
}

#undef INDEX_INVALID
#undef INDEX_INDEXPAGE_ADDRESSITEMS
#undef INDEX_INDEXPAGE_NEXTINDEXPAGE
#undef INDEX_INDEXPAGE_ADDRESSITEMBEGIN
