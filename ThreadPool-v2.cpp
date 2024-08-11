// ThreadPool-v2.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include"threadpool.h"
#include<chrono>
using namespace std;

int sum1(int a, int b)
{
    this_thread::sleep_for(chrono::seconds(2));
    return a + b;
}

int sum2(int a, int b,int c)
{
    this_thread::sleep_for(chrono::seconds(2));
    return a + b + c;
}

int main()
{
    ThreadPool tp;
    //tp.setMode(PoolMode::MODE_CACHED);
    tp.start(2);

    future<int>r1 = tp.submitTask(sum1, 1, 2);
    future<int>r2 = tp.submitTask(sum2, 1, 2,3);
    future<int>r3 = tp.submitTask(sum2, 1, 2,8);
    future<int>r4 = tp.submitTask(sum1, 8, 2);
    future<int>r5 = tp.submitTask(sum1, 100, 300);
    future<int>r6 = tp.submitTask([](int b,int e)->int {
        int sum = 0;
        for (int i = b; i <= e; i++)
        {
            sum += i;
        }
        return sum;
        },1,100 );

    std::cout << r1.get() << std::endl;
    std::cout << r2.get() << std::endl;
    std::cout << r3.get() << std::endl;
    std::cout << r4.get() << std::endl;
    std::cout << r5.get() << std::endl;
    std::cout << r6.get() << std::endl;
}

