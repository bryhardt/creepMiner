//  cryptoport.io Burst Pool Miner
//
//  Created by Uray Meiviar < uraymeiviar@gmail.com > 2014
//  donation :
//
//  [Burst  ] BURST-8E8K-WQ2F-ZDZ5-FQWHX
//  [Bitcoin] 1UrayjqRjSJjuouhJnkczy5AuMqJGRK4b

#include "Miner.h"
#include "MinerLogger.h"
#include "MinerConfig.h"
#include "PlotReader.h"
#include "MinerUtil.h"
#include "nxt/nxt_address.h"
#include <random>
#include <deque>

Burst::Miner::Miner(MinerConfig& config)
	: scoopNum(0), baseTarget(0), blockHeight(0)
{
	this->config = &config;
	this->running = false;
}

Burst::Miner::~Miner()
{}

void Burst::Miner::run()
{
    this->running = true;
    std::thread submitter(&Miner::nonceSubmitterThread,this);
    
	progress = std::make_unique<PlotReadProgress>();

	if(!this->protocol.run(this))
        MinerLogger::write("Mining networking failed", TextType::Error);

    this->running = false;
    submitter.join();
}

void Burst::Miner::stop()
{
	this->running = false;
}

const Burst::MinerConfig* Burst::Miner::getConfig() const
{
    return this->config;
}

void Burst::Miner::updateGensig(const std::string gensigStr, uint64_t blockHeight, uint64_t baseTarget)
{
	// stop all reading processes if any
	for (auto& plotReader : plotReaders)
		plotReader->stop();

    this->gensigStr = gensigStr;
    this->blockHeight = blockHeight;
    this->baseTarget = baseTarget;
    
	for(auto i = 0; i < 32; ++i)
    {
	    auto byteStr = gensigStr.substr(i*2,2);
        this->gensig[i] = static_cast<uint8_t>(std::stoi(byteStr, nullptr, 16));
    }
    
    GensigData newGenSig;
    this->hash.update(&this->gensig[0],this->gensig.size());
    this->hash.update(blockHeight);
	this->hash.close(&newGenSig[0]);
	this->scoopNum = (static_cast<int>(newGenSig[newGenSig.size()-2] & 0x0F) << 8) | static_cast<int>(newGenSig[newGenSig.size()-1]);

	//auto writeLine = [](const std::string& text, uint8_t lineWidth = 76)
	//{
		//std::string filler(lineWidth - text.size(), ' ');
		//MinerLogger::write(text + filler, MinerLogger::TextType::Urgent);
	//};

	//writeLine("block#      " + std::to_string(blockHeight));
	//writeLine("scoop#      " + std::to_string(scoopNum));
	//writeLine("baseTarget# " + std::to_string(baseTarget));
	//writeLine("gensig#     " + gensigStr);

	MinerLogger::write(std::string(76, '-'), TextType::Information);
	MinerLogger::write("block#      " + std::to_string(blockHeight), TextType::Information);
	MinerLogger::write("scoop#      " + std::to_string(scoopNum), TextType::Information);
	MinerLogger::write("baseTarget# " + std::to_string(baseTarget), TextType::Information);
	MinerLogger::write("gensig#     " + gensigStr, TextType::Information);
	MinerLogger::write(std::string(76, '-'), TextType::Information);
    
    //this->bestDeadline.clear();
	this->bestDeadline.clear();
    this->config->rescan();
    this->plotReaders.clear();

	// this block is closed in itself
	// dont use the variables in it outside!
	{
		using PlotList = std::vector<std::shared_ptr<PlotFile>>;
		std::unordered_map<std::string, PlotList> plotDirs;

		for(const auto plotFile : this->config->getPlotFiles())
		{
			auto last_slash_idx = plotFile->getPath().find_first_of('/\\');

			std::string dir;

			if (last_slash_idx == std::string::npos)
				continue;

			dir = plotFile->getPath().substr(0, last_slash_idx);
		
			auto iter = plotDirs.find(dir);

			if (iter == plotDirs.end())
				plotDirs.emplace(std::make_pair(dir, PlotList{}));

			plotDirs[dir].emplace_back(plotFile);
		}
		  
		auto progress = std::make_shared<PlotReadProgress>();
		progress->setMax(config->getTotalPlotsize());

		for (auto& plotDir : plotDirs)
		{
			auto reader = std::make_shared<PlotListReader>(*this, progress);
			reader->read(std::move(plotDir.second));
			plotReaders.emplace_back(reader);
		}
	}
    
    std::random_device rd;
    std::default_random_engine randomizer(rd());
    std::uniform_int_distribution<size_t> dist(0, this->config->submissionMaxDelay);
	auto delay = dist(randomizer);

    this->nextNonceSubmission = std::chrono::system_clock::now() +
                                std::chrono::seconds(delay);
    
	{
		std::lock_guard<std::mutex> mutex(this->accountLock);
		this->bestDeadline.clear();
		this->bestNonce.clear();
		this->bestDeadlineConfirmed.clear();
    }
}

const Burst::GensigData& Burst::Miner::getGensig() const
{
    return this->gensig;
}

uint64_t Burst::Miner::getBaseTarget() const
{
    return this->baseTarget;
}

size_t Burst::Miner::getScoopNum() const
{
    return this->scoopNum;
}

void Burst::Miner::nonceSubmitterThread()
{
    std::deque<std::pair<uint64_t,uint64_t>> submitList;
    std::unique_lock<std::mutex> mutex(this->accountLock);
    mutex.unlock();

    while(this->running)
    {
        if(std::chrono::system_clock::now() > this->nextNonceSubmission ||
			std::all_of(plotReaders.begin(), plotReaders.end(), [](std::shared_ptr<PlotListReader> reader) { return reader->isDone(); }))
        {
            mutex.lock();

            for (auto& kv : bestDeadline)
            {
	            auto accountId = kv.first;
	            auto deadline = kv.second;
	            auto nonce = this->bestNonce[accountId];
	            auto firstSubmit = this->bestDeadlineConfirmed.find(accountId) == this->bestDeadlineConfirmed.end();

				if (!firstSubmit)
					firstSubmit = deadline < this->bestDeadlineConfirmed[accountId];

                if( firstSubmit )
                {
	                auto exist = false;

					for (size_t i = 0; i < submitList.size(); i++)
					{
						if (submitList.at(i).first == nonce &&
							submitList.at(i).second == accountId)
						{
							exist = true;
							break;
						}
					}

					if (!exist)
						submitList.emplace_back(nonce, accountId);
                }
            }

            mutex.unlock();
            
            std::random_device rd;
            std::default_random_engine randomizer(rd());
            std::uniform_int_distribution<size_t> dist(0, this->config->submissionMaxDelay);
            this->nextNonceSubmission = std::chrono::system_clock::now() + std::chrono::seconds(dist(randomizer));
            
            if(submitList.size() > 0)
            {
				mutex.lock();
				
				auto submitData = submitList.front();
				submitList.pop_front();
				
				mutex.unlock();

	            auto deadline = bestDeadline[submitData.second];

                if(protocol.submitNonce(submitData.first, submitData.second, deadline) == SubmitResponse::Submitted)
					this->nonceSubmitReport(submitData.first, submitData.second, deadline);
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void Burst::Miner::submitNonce(uint64_t nonce, uint64_t accountId, uint64_t deadline)
{
    std::lock_guard<std::mutex> mutex(this->accountLock);
	NxtAddress addr(accountId);

    if(this->bestDeadline.find(accountId) != this->bestDeadline.end())
    {
        if(deadline < this->bestDeadline[accountId])
        {
            this->bestDeadline[accountId] = deadline;
            this->bestNonce[accountId] = nonce;

			MinerLogger::write(addr.to_string() + ": deadline found (" + Burst::deadlineFormat(deadline) + ")", TextType::Ok);
        }
    }
    else
    {
        this->bestDeadline.insert(std::make_pair(accountId, deadline));
        this->bestNonce.insert(std::make_pair(accountId, nonce));

		MinerLogger::write(addr.to_string() + ": deadline found (" + Burst::deadlineFormat(deadline) + ")", TextType::Ok);
    }
}

void Burst::Miner::nonceSubmitReport(uint64_t nonce, uint64_t accountId, uint64_t deadline)
{
    std::lock_guard<std::mutex> mutex(this->accountLock);
	NxtAddress addr(accountId);
    
	if(this->bestDeadlineConfirmed.find(accountId) != this->bestDeadlineConfirmed.end())
    {
        if(deadline < this->bestDeadlineConfirmed[accountId])
        {
            this->bestDeadlineConfirmed[accountId] = deadline;
			MinerLogger::write(addr.to_string() + ": deadline confirmed (" + deadlineFormat(deadline) + ")", TextType::Success);
        }
    }
    else
    {
        this->bestDeadlineConfirmed.insert(std::make_pair(accountId, deadline));
		MinerLogger::write(addr.to_string() + ": deadline confirmed (" + deadlineFormat(deadline) + ")", TextType::Success);
    }
}