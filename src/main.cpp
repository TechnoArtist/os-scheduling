#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include "../include/configreader.h"
#include "../include/process.h"

// May change any code as much as we like to get things done, 
// but probably will mostly change main only. 
// Team emails: krue2303, piet9564
// 	Note: Scheduler data already contains a mutex for locking, use unique_lock? (repeatable)
// Note: each process stores its own priority, start time, state, and other info

// Shared data for all cores
typedef struct SchedulerData {
	std::mutex mutex;
	std::condition_variable condition;
	ScheduleAlgorithm algorithm;
	uint32_t context_switch;
	uint32_t time_slice;
	std::list<Process*> ready_queue;
	bool all_terminated;
	Process** running_array; 
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex);
void clearOutput(int num_lines);
uint64_t currentTime();
std::string processStateToString(Process::State state);

int main(int argc, char **argv)
{
	// ----------------- Start input checking and variable declaration ------------------
	
	// Ensure user entered a command line parameter for configuration file name
	if (argc < 2)
	{
		std::cerr << "Error: must specify configuration file" << std::endl;
		exit(EXIT_FAILURE);
	}

	// Declare variables used throughout main
	int i;
	SchedulerData *shared_data;
	std::vector<Process*> processes;

	//variables for final statistics
	uint64_t finish = 0;
	uint64_t totalTime = 0;
	uint64_t averageThroughput = 0;
	uint64_t totalFirst50 = 0;
	uint64_t averageFirst50 = 0;
	uint64_t totalLast50 = 0;
	uint64_t averageLast50 = 0;
	double totalTurnaroundTime = 0;
	double averageTurnaroundTime = 0;
	double totalWaitTime = 0;
	double averageWaitTime = 0;
	int number_processes = 0;
	uint64_t totalCpuTime = 0;
	uint64_t utilTime = 0;
	
	uint64_t now; 
	
	struct SjfComparator sjfcomparator; 
	struct PpComparator ppcomparator; 
	
	// ----------------- Finished input checking and variable declaration ------------------
	// ----------------- Start parsing config file ------------------
	
	// Read configuration file for scheduling simulation
	SchedulerConfig *config = readConfigFile(argv[1]);

	// Store configuration parameters in shared data object
	uint8_t num_cores = config->cores;
	shared_data = new SchedulerData();
	shared_data->algorithm = config->algorithm;
	shared_data->context_switch = config->context_switch;
	shared_data->time_slice = config->time_slice;
	shared_data->all_terminated = false;

	// Create processes
	uint64_t start = currentTime();
	for (i = 0; i < config->num_processes; i++)
	{
		Process *p = new Process(config->processes[i], start);
		processes.push_back(p);
		// If process should be launched immediately, add to ready queue
		if (p->getState() == Process::State::Ready)
		{
			shared_data->ready_queue.push_back(p);
		}
	}

	number_processes = config->num_processes;
	
	shared_data->running_array = (Process**)malloc(sizeof(Process*) * num_cores); 

	// Free configuration data from memory
	deleteConfig(config);
	int d = 3;
	d++; 
	
	free(shared_data->running_array); 
	
	// ----------------- Finished parsing config file ------------------
	// ----------------- Start main work ------------------

	// Launch 1 scheduling thread per cpu core
	std::thread *schedule_threads = new std::thread[num_cores];
	for (i = 0; i < num_cores; i++)
	{
		schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
	}

	// Main thread work goes here
	int num_lines = 0;
	Process* proc; 
	//std::unique_lock<std::mutex> lock(shared_data->mutex);
	while (!(shared_data->all_terminated))
	{
		// Clear output from previous iteration
		clearOutput(num_lines);

		// Do the following:
		
		//   - Get current time
		now = currentTime(); 
		
		//   - *Check if any processes need to move from NotStarted to Ready (based on elapsed time), and if so put that process in the ready queue
		//			NOTE the processes have start times that can be checked
		for(int i = 0; i < processes.size(); i++) {
			proc = processes.at(i); 
			if(proc->getState() == Process::State::NotStarted) {
				if(proc->getStartTime() <= now) {
					//while(!lock.try_lock()) {}
					proc->setState(Process::State::Ready, now); 
					shared_data->ready_queue.push_back(proc);
					//lock.unlock(); 
				}
			}
		}
		
		//   - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
		for(int i = 0; i < processes.size(); i++) {
			proc = processes.at(i); 
			if(now - proc->getBurstStartTime() >= proc->getCurrentBurstTime()) {
				proc->updateProcess(now); 
				proc->setState(Process::State::Ready, now); 
				//lock.lock(); 
				shared_data->ready_queue.push_back(proc); 
				//lock.unlock(); 
				proc->updateCurrentBurst(); 
			}
			
		}
		
		//   - *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
		if(shared_data->algorithm == ScheduleAlgorithm::RR) {
			//while(!lock.try_lock()) {}
			Process* current_proc = processes.at(0); 
			for(int i = 0; i < num_cores; i++) { 
				current_proc = shared_data->running_array[i]; 
				if(current_proc->getState() == Process::Running && (now - current_proc->getBurstStartTime()) >= shared_data->time_slice) { 
					//If round robin, and time slice expired...
					current_proc->interrupt(); 
					shared_data->ready_queue.push_back(current_proc); 
				}
			}
			//lock.unlock(); 
		}
		
		if(shared_data->algorithm == ScheduleAlgorithm::PP) {
			//while(!lock.try_lock()) {}
			Process* current_proc = processes.at(0);
			for(int i = 0; i < num_cores; i++) {
				//iterate through ready queue, comparing priority with any running processes
				current_proc = shared_data->running_array[i]; 
				if(current_proc->getState() == Process::Running) {
					for(int j = 0; j < shared_data->ready_queue.size(); j++){
						proc = processes.at(j); 
						if(current_proc->getPriority() < proc->getPriority()){
							//If preemptive priority, and queue has > current...
							current_proc->interrupt();
							shared_data->ready_queue.push_back(current_proc);
						}
					}
				}
			}
			//lock.unlock(); 
			
		}
		
		//   - *Sort the ready queue (if needed - based on scheduling algorithm)
		if(shared_data->algorithm == ScheduleAlgorithm::SJF) {
			//while(!lock.try_lock()) {}
			shared_data->ready_queue.sort(sjfcomparator); 
			//lock.unlock(); 
		}
		if(shared_data->algorithm == ScheduleAlgorithm::PP) { 
			//while(!lock.try_lock()) {}
			shared_data->ready_queue.sort(ppcomparator); 
			//lock.unlock(); 
		}
		
		//   - Determine if all processes are in the terminated state
		//   - * = accesses shared data (ready queue), so be sure to use proper synchronization
		//while(!lock.try_lock()) {}
		int alldone = 1; 
		for(int i = 0; i < processes.size(); i++) {
			proc = processes.at(i); 
			if(proc->getState() != Process::State::Terminated) {
				alldone = 0; 
			}
		}
		if(alldone == 1) shared_data->all_terminated = true; 
		//lock.unlock(); 
		

		// output process status table
		num_lines = printProcessOutput(processes, shared_data->mutex);

		// sleep 50 ms
		usleep(50000);
	}


	// wait for threads to finish
	for (i = 0; i < num_cores; i++)
	{
		schedule_threads[i].join();
	}

	//calculations for final statistics
	finish = currentTime();
	totalTime = start - finish;

	//calculate throughput statistics
	averageThroughput = totalTime/ number_processes;
	int firstHalfNum = 0;
	int secondHalfNum = 0;
	firstHalfNum = number_processes / 2;
	secondHalfNum = firstHalfNum;
	//compenstate for odd total number of processes
	if(number_processes % 2 != 0){
		firstHalfNum = firstHalfNum + 1;
	}
	
	//note: I think you can get the num processes from processes.size()
	averageFirst50 = totalFirst50 / firstHalfNum;
	averageLast50 = averageLast50 / secondHalfNum;

	//calculate CPU utilization (% of time CPU is actually computing)
	for(int j = 0; j < number_processes; j++){
		Process* proc1 = processes.at(j);
		totalCpuTime = totalCpuTime + proc1->getCpuTime();
	}
	utilTime = totalCpuTime / totalTime;
	//to make a percentage
	utilTime = utilTime * 100;

	//calculate average turnaround time
	for(int j = 0; j < number_processes; j++){
		Process* proc1 = processes.at(j);
		totalTurnaroundTime = totalTurnaroundTime + proc1->getTurnaroundTime();
	}
	averageTurnaroundTime = totalTurnaroundTime / number_processes;

	//calculate average wait time
	for(int j = 0; j < number_processes; j++){
		Process* proc1 = processes.at(j);
		totalWaitTime = totalWaitTime + proc1->getWaitTime();
	}
	averageWaitTime = totalWaitTime / number_processes;

	// calculate and print final statistics (not the printProcessOutput table)
	//  - CPU utilization (executing vs idle; ready, or switching, are idle)
	printf("CPU Utilization: %lui", utilTime);
	//  - Throughput (how long to finish...)
	//	 - Average for first 50% of processes finished
	printf("First 50 Percent Average Throughput: %lui", averageFirst50);
	//	 - Average for second 50% of processes finished
	printf("Last 50 Percent Average Throughput: %lui", averageLast50);
	//	 - Overall average
	printf("Overall Average Throughput: %lui", averageThroughput);
	//  - Average turnaround time (how long to finish individual)
	printf("Average Turnaround Time: %f", averageTurnaroundTime);
	//  - Average waiting time (how much time spent in ready queue)
	printf("Average Wait Time: %f", averageWaitTime);

	// Clean up before quitting program
	processes.clear();
	
	// ----------------- Finished main work ------------------

	return 0;
}

void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{
	Process* current_proc; 
	uint64_t now; 
	//std::unique_lock<std::mutex> lock(shared_data->mutex);
	
	// Work to be done by each core independent of the other cores
	// Repeat until all processes in terminated state:
	while(!(shared_data->all_terminated)) {
		now = currentTime(); 
		//   - *Get process at front of ready queue
		//while(!lock.try_lock()) {}
		current_proc = *shared_data->ready_queue.begin(); //how to get an item from the ready queue? 
		shared_data->ready_queue.pop_front(); 
		current_proc->updateProcess(now); 
		current_proc->setState(Process::Running, now); 
		shared_data->running_array[core_id] = current_proc; 
		//lock.unlock(); 
		
		//   - Simulate the processes running until one of the following:
		while(true) {
			//	 - CPU burst time has elapsed
			if(currentTime() - current_proc->getBurstStartTime() >= current_proc->getCurrentBurstTime()) {
				current_proc->updateCurrentBurst(); 
				current_proc->updateProcess(now); 
				break; 
			}
			
			//	 - Interrupted (RR time slice has elapsed or process preempted by higher priority process)
			if(current_proc->isInterrupted()) {
				break; 
			}
			
			//			NOTE don't sleep for too long, in case of interrupt etc
			usleep(20000); 
		}
		
		now = currentTime(); 
		
		//  - Place the process back in the appropriate queue
		//	 - I/O queue if CPU burst finished (and process not finished) -- no actual queue, simply set state to IO
		if(!current_proc->isInterrupted() && current_proc->getRemainingTime() > 0) {
			current_proc->updateProcess(now); 
			current_proc->setState(Process::IO, now); 
			current_proc->updateCurrentBurst(); 
		}
		//	 - Terminated if CPU burst finished and no more bursts remain -- no actual queue, simply set state to Terminated
		if(current_proc->getRemainingTime() <= 0) {
			current_proc->updateProcess(now); 
			current_proc->setState(Process::Terminated, now); 
		}
		//	 - *Ready queue if interrupted (be sure to modify the CPU burst time to now reflect the remaining time)
		if(current_proc->isInterrupted()) {
			current_proc->updateProcess(now); 
			current_proc->setState(Process::Ready, now); 
			current_proc->updateCurrentBurstTime(current_proc->getCurrentBurstTime() - (now - current_proc->getBurstStartTime())); 
			current_proc->updateCurrentBurst(); 
			current_proc->interruptHandled(); 
			//while(!lock.try_lock()) {}
			shared_data->ready_queue.push_back(current_proc); 
			//lock.unlock(); 
		}
		
		//  - Wait context switching time
		usleep(shared_data->context_switch * 1000); 
		
		//  - * = accesses shared data (ready queue), so be sure to use proper synchronization
		
		// remove current proc from the running variable
		//while(!lock.try_lock()) {}
		shared_data->running_array[core_id] = 0; 
		//lock.unlock(); 
	}
}

int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex)
{
	int i;
	int num_lines = 2;
	std::lock_guard<std::mutex> lock(mutex);
	printf("|   PID | Priority |	  State | Core | Turn Time | Wait Time | CPU Time | Remain Time |\n");
	printf("+-------+----------+------------+------+-----------+-----------+----------+-------------+\n");
	for (i = 0; i < processes.size(); i++)
	{
		if (processes[i]->getState() != Process::State::NotStarted)
		{
			uint16_t pid = processes[i]->getPid();
			uint8_t priority = processes[i]->getPriority();
			std::string process_state = processStateToString(processes[i]->getState());
			int8_t core = processes[i]->getCpuCore();
			std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";
			double turn_time = processes[i]->getTurnaroundTime();
			double wait_time = processes[i]->getWaitTime();
			double cpu_time = processes[i]->getCpuTime();
			double remain_time = processes[i]->getRemainingTime();
			printf("| %5u | %8u | %10s | %4s | %9.1lf | %9.1lf | %8.1lf | %11.1lf |\n", 
				   pid, priority, process_state.c_str(), cpu_core.c_str(), turn_time, 
				   wait_time, cpu_time, remain_time);
			num_lines++;
		}
	}
	return num_lines;
}

void clearOutput(int num_lines)
{
	int i;
	for (i = 0; i < num_lines; i++)
	{
		fputs("\033[A\033[2K", stdout);
	}
	rewind(stdout);
	fflush(stdout);
}

uint64_t currentTime()
{
	uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				  std::chrono::system_clock::now().time_since_epoch()).count();
	return ms;
}

std::string processStateToString(Process::State state)
{
	std::string str;
	switch (state)
	{
		case Process::State::NotStarted:
			str = "not started";
			break;
		case Process::State::Ready:
			str = "ready";
			break;
		case Process::State::Running:
			str = "running";
			break;
		case Process::State::IO:
			str = "i/o";
			break;
		case Process::State::Terminated:
			str = "terminated";
			break;
		default:
			str = "unknown";
			break;
	}
	return str;
}
