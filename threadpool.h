#ifndef  THREADPOOL_H
#define  THREADPOOL_H

#include<iostream>
#include<vector>
#include<queue>
#include<memory>
#include<atomic>
#include<mutex>
#include<condition_variable>
#include<functional>
#include<unordered_map> 
#include<thread>
#include<future>

const size_t TASK_MAX_THREAD_HOLD = 2;//INT32_MAX;
const size_t THREAD_MAX_THREADHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;  //单位是秒

//线程池支持的两种模式
enum class PoolMode
{
	MODE_FIXED,		//固定大小线程池
	MODE_CACHED,	//可变大小线程池
};

//线程类型
class Thread
{
public:
	//线程函数对象类型
	using ThreadFunc = std::function<void(int)>;

	Thread(ThreadFunc func)
		:func_(func)
		, threadId_(generateId_++)
	{}
	~Thread() = default;
	void start()
	{	
		//创建线程来执行线程函数 func_
		std::thread t(func_, threadId_);
		t.detach();
	}
	int getId() const
	{
		return threadId_;
	}
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_;

};

int Thread::generateId_ = 0;

//线程池类型
class ThreadPool
{
public:
	ThreadPool()
		: initThreadSize_(0)
		, taskSize_(0)
		, idleThreadSize_(0)
		, curThreadSize_(0)
		, taskQueMaxThreadHold_(TASK_MAX_THREAD_HOLD)
		, threadSizeThreadHold_(THREAD_MAX_THREADHOLD)
		, poolMode_(PoolMode::MODE_FIXED)
		, isPoolRunning_(false)
		{}
	~ThreadPool()
		{
			isPoolRunning_ = false;

			std::unique_lock<std::mutex> lock(taskQueMtx_);
			notEmpty_.notify_all();

			exitCond_.wait(lock, [&]()->bool {
				return threads_.size() == 0;
				});

		}
	//设置线程池模式
	void setMode(PoolMode mode)
	{
		if (checkRunningState())
		{
			return;
		}
		poolMode_ = mode;
	}
	////设置初始线程数量
	//void setInitThreadSize(int size);
	//设置任务队列上限
	void setTaskQueMaxThreadHold(int threshhold)
	{
		//不允许启动线程池后修改队列阈值
		if (checkRunningState())
		{
			return;
		}

		taskQueMaxThreadHold_ = threshhold;
	}
	//设置cache模式下线程数量的阈值
	void setThreadSizeThreadHold(int threshhold)
	{
		if (checkRunningState())
		{
			return;
		}
		if (poolMode_ == PoolMode::MODE_CACHED)
		{
			threadSizeThreadHold_ = threshhold;
		}

	}

	//提交任务
	//使用可变参模板编程  让submitTask接受任意函数和任意参数
	template<typename Func,typename... Args>
	auto submitTask(Func&& func, Args&&...args) -> std::future<decltype(func(args...))>
	{
		//打包任务 放入队列中
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
			);
		std::future<RType> result = task->get_future();


		//获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		//线程通信 队列中任务满了,生产者线程阻塞,等待消费者线程消费任务
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]() {return taskQue_.size() < taskQueMaxThreadHold_; }))
		{
			//notFull，条件依然不满足
			std::cerr << "task queue is full submit task fail .ThreadPool::submitTask timeout" << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType {return RType(); }
				);
			(*task)();
			return task->get_future();
		}

		//如果有队列有空余,生产者线程生产任务并将任务放入队列中
		//taskQue_.emplace(sp);
		//using Task = std::function<void()>;
		taskSize_++;
		taskQue_.emplace([task]() {
			(*task)();
			});


		//因为已经向队列中放入了任务,任务队列不为空,notEmpty_条件变量通知消费者线程赶快分配线程执行任务
		notEmpty_.notify_all();

		//cache模式 任务处理比较紧急 场景:小而快
		//根据任务数量和空闲线程的数量,判断是否需要创建新的线程
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadSizeThreadHold_)
		{

			std::cout << "<<< tid " << std::this_thread::get_id() << " create new thread " << curThreadSize_ << std::endl;

			//创建新线程
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			//启动线程
			threads_[threadId]->start();
			//当前线程数量+1 
			curThreadSize_++;
			//记录空闲线程数量
			idleThreadSize_++;
		}


		//返回任务的Result对象
		//return task->getResult();

		return result;

	}
	//启动线程池
	void start(int initThreadSize = std::thread::hardware_concurrency())
	{
		//设置线程池的运行状态
		isPoolRunning_ = true;

		//记录初始线程数量
		initThreadSize_ = initThreadSize;

		//记录当前线程数量
		curThreadSize_ = initThreadSize;

		for (int i = 0; i < initThreadSize_; i++)
		{
			//std::unique_ptr<Thread> ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc,this,std::placeholders::_1));
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			//threads_.emplace_back(std::move(ptr));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
		}
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start();  //
			idleThreadSize_++;	   //记录空闲的线程数量
		}

	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	//线程函数
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock::now();

		//所有任务必须完全执行完之后,线程池才可以回收所有线程资源
		for (;;)
		{

			Task task;
			{
				//先获取锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid " << std::this_thread::get_id() << " Attempt to Acquire Task..." << std::endl;

				//cache模式下 有可能创建了很多线程 但是空闲时间超过了60s 应该把多余的线程结束回收掉
				//结束回收掉(超过intiThreadSize_数量的线程要进行回收)
				//当前时间 - 上一次线程执行任务的时间   >  60s


					//每一秒钟返回一次  怎么区分超时返回还是有任务待执行返回
					//锁加双重判断
				while (taskQue_.size() == 0)
				{
					if (!isPoolRunning_)
					{
						threads_.erase(threadid);
						std::cout << "tid" << std::this_thread::get_id() << " Thread Exit..." << std::endl;
						exitCond_.notify_all();
						return;//线程函数结束 线程退出
					}


					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						//条件变量超时返回
						if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock::now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
							{

								//开始回收当前线程
								//记录线程数量的相关变量的值修改
								//把线程对象从线程池中删除 没有办法匹配当前的threadFunc 属于哪一个线程对象
								//线程id ->线程对象->删除
								threads_.erase(threadid);
								curThreadSize_--;
								idleThreadSize_--;
								std::cout << "tid" << std::this_thread::get_id() << " Thread Execution Timeout Reclamation..." << std::endl;
								return;

							}

						}
					}
					else
					{
						//等待notEmpty条件
						//1.先判断任务队列是否为空,为空,则线程阻塞等待
						notEmpty_.wait(lock);
					}			
				}
				//任务队列不为空，则线程从队列中取任务执行 空闲线程数量减一
				idleThreadSize_--;

				//std::cout << "tid" << std::this_thread::get_id() << " 任务成功..." << std::endl;

				//任务队列不为空,则线程从队列中取任务执行
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				//如果还有任务,则通知消费者线程
				if (taskSize_ > 0)
				{
					notEmpty_.notify_all();
				}

				//线程执行完任务,则通知生产者线程,队列中还有任务
				notFull_.notify_all();

			}//释放锁 

			//4.执行任务
			if (task != nullptr)
			{
				//task->run();//执行任务 把任务的返回值通过serVal设置到Result对象中
				//task->exec();
				task();//执行function<void()>
			}
			idleThreadSize_++;//线程执行完任务,空闲线程数量加一
			lastTime = std::chrono::high_resolution_clock::now();//更新线程执行完任务的时间

		}

	}

	//检查pool的运行状态
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}
private:
	//std::vector<std::unique_ptr<Thread>> threads_;//线程列表
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;//线程列表
	size_t initThreadSize_;//初始线程数量
	size_t threadSizeThreadHold_;//线程数量上限阈值
	std::atomic_size_t curThreadSize_; //记录线程池里线程的数量
	std::atomic_size_t idleThreadSize_;//记录空闲线程数量
	//std::atomic_size_t threadCount_;


	//Task任务=>函数对象
	using Task = std::function<void()>;
	std::queue <Task> taskQue_;//任务队列
	std::atomic_size_t taskSize_;		//任务数量
	size_t taskQueMaxThreadHold_;     //任务队列数量上限阈值

	std::mutex taskQueMtx_; //任务队列互斥锁
	std::condition_variable notFull_;//队列 不满 条件变量
	std::condition_variable notEmpty_;//队列 不空 条件变量
	std::condition_variable exitCond_;//队列 不空 条件变量

	PoolMode poolMode_;//线程池模式

	std::atomic_int isPoolRunning_; //表示线程的启动状态


};


#endif // !



