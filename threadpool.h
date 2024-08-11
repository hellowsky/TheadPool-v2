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
const int THREAD_MAX_IDLE_TIME = 60;  //��λ����

//�̳߳�֧�ֵ�����ģʽ
enum class PoolMode
{
	MODE_FIXED,		//�̶���С�̳߳�
	MODE_CACHED,	//�ɱ��С�̳߳�
};

//�߳�����
class Thread
{
public:
	//�̺߳�����������
	using ThreadFunc = std::function<void(int)>;

	Thread(ThreadFunc func)
		:func_(func)
		, threadId_(generateId_++)
	{}
	~Thread() = default;
	void start()
	{	
		//�����߳���ִ���̺߳��� func_
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

//�̳߳�����
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
	//�����̳߳�ģʽ
	void setMode(PoolMode mode)
	{
		if (checkRunningState())
		{
			return;
		}
		poolMode_ = mode;
	}
	////���ó�ʼ�߳�����
	//void setInitThreadSize(int size);
	//���������������
	void setTaskQueMaxThreadHold(int threshhold)
	{
		//�����������̳߳غ��޸Ķ�����ֵ
		if (checkRunningState())
		{
			return;
		}

		taskQueMaxThreadHold_ = threshhold;
	}
	//����cacheģʽ���߳���������ֵ
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

	//�ύ����
	//ʹ�ÿɱ��ģ����  ��submitTask�������⺯�����������
	template<typename Func,typename... Args>
	auto submitTask(Func&& func, Args&&...args) -> std::future<decltype(func(args...))>
	{
		//������� ���������
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
			);
		std::future<RType> result = task->get_future();


		//��ȡ��
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		//�߳�ͨ�� ��������������,�������߳�����,�ȴ��������߳���������
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]() {return taskQue_.size() < taskQueMaxThreadHold_; }))
		{
			//notFull��������Ȼ������
			std::cerr << "task queue is full submit task fail .ThreadPool::submitTask timeout" << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType {return RType(); }
				);
			(*task)();
			return task->get_future();
		}

		//����ж����п���,�������߳��������񲢽�������������
		//taskQue_.emplace(sp);
		//using Task = std::function<void()>;
		taskSize_++;
		taskQue_.emplace([task]() {
			(*task)();
			});


		//��Ϊ�Ѿ�������з���������,������в�Ϊ��,notEmpty_��������֪ͨ�������̸߳Ͽ�����߳�ִ������
		notEmpty_.notify_all();

		//cacheģʽ ������ȽϽ��� ����:С����
		//�������������Ϳ����̵߳�����,�ж��Ƿ���Ҫ�����µ��߳�
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadSizeThreadHold_)
		{

			std::cout << "<<< tid " << std::this_thread::get_id() << " create new thread " << curThreadSize_ << std::endl;

			//�������߳�
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			//�����߳�
			threads_[threadId]->start();
			//��ǰ�߳�����+1 
			curThreadSize_++;
			//��¼�����߳�����
			idleThreadSize_++;
		}


		//���������Result����
		//return task->getResult();

		return result;

	}
	//�����̳߳�
	void start(int initThreadSize = std::thread::hardware_concurrency())
	{
		//�����̳߳ص�����״̬
		isPoolRunning_ = true;

		//��¼��ʼ�߳�����
		initThreadSize_ = initThreadSize;

		//��¼��ǰ�߳�����
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
			idleThreadSize_++;	   //��¼���е��߳�����
		}

	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	//�̺߳���
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock::now();

		//�������������ȫִ����֮��,�̳߳زſ��Ի��������߳���Դ
		for (;;)
		{

			Task task;
			{
				//�Ȼ�ȡ��
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid " << std::this_thread::get_id() << " Attempt to Acquire Task..." << std::endl;

				//cacheģʽ�� �п��ܴ����˺ܶ��߳� ���ǿ���ʱ�䳬����60s Ӧ�ðѶ�����߳̽������յ�
				//�������յ�(����intiThreadSize_�������߳�Ҫ���л���)
				//��ǰʱ�� - ��һ���߳�ִ�������ʱ��   >  60s


					//ÿһ���ӷ���һ��  ��ô���ֳ�ʱ���ػ����������ִ�з���
					//����˫���ж�
				while (taskQue_.size() == 0)
				{
					if (!isPoolRunning_)
					{
						threads_.erase(threadid);
						std::cout << "tid" << std::this_thread::get_id() << " Thread Exit..." << std::endl;
						exitCond_.notify_all();
						return;//�̺߳������� �߳��˳�
					}


					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						//����������ʱ����
						if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock::now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
							{

								//��ʼ���յ�ǰ�߳�
								//��¼�߳���������ر�����ֵ�޸�
								//���̶߳�����̳߳���ɾ�� û�а취ƥ�䵱ǰ��threadFunc ������һ���̶߳���
								//�߳�id ->�̶߳���->ɾ��
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
						//�ȴ�notEmpty����
						//1.���ж���������Ƿ�Ϊ��,Ϊ��,���߳������ȴ�
						notEmpty_.wait(lock);
					}			
				}
				//������в�Ϊ�գ����̴߳Ӷ�����ȡ����ִ�� �����߳�������һ
				idleThreadSize_--;

				//std::cout << "tid" << std::this_thread::get_id() << " ����ɹ�..." << std::endl;

				//������в�Ϊ��,���̴߳Ӷ�����ȡ����ִ��
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				//�����������,��֪ͨ�������߳�
				if (taskSize_ > 0)
				{
					notEmpty_.notify_all();
				}

				//�߳�ִ��������,��֪ͨ�������߳�,�����л�������
				notFull_.notify_all();

			}//�ͷ��� 

			//4.ִ������
			if (task != nullptr)
			{
				//task->run();//ִ������ ������ķ���ֵͨ��serVal���õ�Result������
				//task->exec();
				task();//ִ��function<void()>
			}
			idleThreadSize_++;//�߳�ִ��������,�����߳�������һ
			lastTime = std::chrono::high_resolution_clock::now();//�����߳�ִ���������ʱ��

		}

	}

	//���pool������״̬
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}
private:
	//std::vector<std::unique_ptr<Thread>> threads_;//�߳��б�
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;//�߳��б�
	size_t initThreadSize_;//��ʼ�߳�����
	size_t threadSizeThreadHold_;//�߳�����������ֵ
	std::atomic_size_t curThreadSize_; //��¼�̳߳����̵߳�����
	std::atomic_size_t idleThreadSize_;//��¼�����߳�����
	//std::atomic_size_t threadCount_;


	//Task����=>��������
	using Task = std::function<void()>;
	std::queue <Task> taskQue_;//�������
	std::atomic_size_t taskSize_;		//��������
	size_t taskQueMaxThreadHold_;     //�����������������ֵ

	std::mutex taskQueMtx_; //������л�����
	std::condition_variable notFull_;//���� ���� ��������
	std::condition_variable notEmpty_;//���� ���� ��������
	std::condition_variable exitCond_;//���� ���� ��������

	PoolMode poolMode_;//�̳߳�ģʽ

	std::atomic_int isPoolRunning_; //��ʾ�̵߳�����״̬


};


#endif // !



