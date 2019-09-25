// WorkloadCharacterisation.cpp (Oclgrind)
// Copyright (c) 2017, Beau Johnston,
// The Australian National University. All rights reserved.
//
// This program is provided under a three-clause BSD license. For full
// license terms please see the LICENSE file distributed with this
// source code.

#include "core/common.h"

#include <algorithm>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <math.h>
#include <numeric>
#include <sstream>
#include <vector>

#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"

#include "WorkloadCharacterisation.h"

#include "core/Kernel.h"
#include "core/KernelInvocation.h"
#include "core/Memory.h"
#include "core/WorkGroup.h"
#include "core/WorkItem.h"

using namespace oclgrind;
using namespace std;

#define COUNTED_LOAD_BASE (llvm::Instruction::OtherOpsEnd + 4)
#define COUNTED_STORE_BASE (COUNTED_LOAD_BASE + 8)
#define COUNTED_CALL_BASE (COUNTED_STORE_BASE + 8)

THREAD_LOCAL WorkloadCharacterisation::WorkerState
    WorkloadCharacterisation::m_state = {NULL};

WorkloadCharacterisation::WorkloadCharacterisation(const Context *context) : WorkloadCharacterisation::Plugin(context) {
  m_numberOfHostToDeviceCopiesBeforeKernelNamed = 0;
  m_last_kernel_name = "";
}

WorkloadCharacterisation::~WorkloadCharacterisation() {
  locale previousLocale = cout.getloc();
  locale defaultLocale("");
  cout.imbue(defaultLocale);

  // present memory transfer statistics -- only run once, since these are collected outside kernel invocations
  cout << "+-------------------------------------------------------------------------------------------------------+" << endl;
  cout << "|Memory Transfers -- statistics around host to device and device to host memory transfers               |" << endl;
  cout << "+=======================================================================================================+" << endl;
  // I can't imagine a scenario where data are copied from the device before a kernel is executed. So I use the deviceToHostCopy kernel names for the final statistics -- these names for the m_hostToDeviceCopy's are updated when the kernel is enqueued.
  std::vector<std::string> x = m_deviceToHostCopy;
  std::vector<std::string>::iterator unique_x = std::unique(x.begin(), x.end());
  x.resize(std::distance(x.begin(), unique_x));
  std::vector<std::string> unique_kernels_involved_with_device_to_host_copies = x;

  x = m_hostToDeviceCopy;
  unique_x = std::unique(x.begin(), x.end());
  x.resize(std::distance(x.begin(), unique_x));
  std::vector<std::string> unique_kernels_involved_with_host_to_device_copies = x;

  cout << "Total Host To Device Transfers (#) for kernel:" << endl;
  for (auto const &item : unique_kernels_involved_with_host_to_device_copies) {
    cout << "\t" << item << ": " << std::count(m_hostToDeviceCopy.begin(), m_hostToDeviceCopy.end(), item) << endl;
  }
  cout << "Total Device To Host Transfers (#) for kernel:" << endl;
  for (auto const &item : unique_kernels_involved_with_device_to_host_copies) {
    cout << "\t" << item << ": " << std::count(m_deviceToHostCopy.begin(), m_deviceToHostCopy.end(), item) << endl;
  }

  //write it out to special .csv file
  int logfile_count = 0;
  std::string logfile_name = "aiwc_memory_transfers_" + std::to_string(logfile_count) + ".csv";
  while (std::ifstream(logfile_name)) {
    logfile_count++;
    logfile_name = "aiwc_memory_transfers_" + std::to_string(logfile_count) + ".csv";
  }
  std::ofstream logfile;
  logfile.open(logfile_name);
  assert(logfile);
  logfile << "metric,kernel,count\n";

  for (auto const &item : unique_kernels_involved_with_host_to_device_copies) {
    logfile << "transfer: host to device," << item << "," << std::count(m_hostToDeviceCopy.begin(), m_hostToDeviceCopy.end(), item) << "\n";
  }
  for (auto const &item : unique_kernels_involved_with_device_to_host_copies) {
    logfile << "transfer: device to host," << item << "," << std::count(m_deviceToHostCopy.begin(), m_deviceToHostCopy.end(), item) << "\n";
  }
  logfile.close();

  // Restore locale
  cout.imbue(previousLocale);
}

void WorkloadCharacterisation::hostMemoryLoad(const Memory *memory, size_t address, size_t size) {
  //device to host copy -- synchronization
  m_deviceToHostCopy.push_back(m_last_kernel_name);
}

void WorkloadCharacterisation::hostMemoryStore(const Memory *memory, size_t address, size_t size, const uint8_t *storeData) {
  //host to device copy -- synchronization
  m_hostToDeviceCopy.push_back(m_last_kernel_name);
  m_numberOfHostToDeviceCopiesBeforeKernelNamed++;
}

void WorkloadCharacterisation::memoryLoad(const Memory *memory, const WorkItem *workItem, size_t address, size_t size) {
  if (memory->getAddressSpace() != AddrSpacePrivate) {
    m_state.memoryOps->push_back(address);
  }
}

void WorkloadCharacterisation::memoryStore(const Memory *memory, const WorkItem *workItem, size_t address, size_t size, const uint8_t *storeData) {
  if (memory->getAddressSpace() != AddrSpacePrivate) {
    m_state.memoryOps->push_back(address);
  }
}

void WorkloadCharacterisation::memoryAtomicLoad(const Memory *memory, const WorkItem *workItem, AtomicOp op, size_t address, size_t size) {
  if (memory->getAddressSpace() != 0) {
    m_state.memoryOps->push_back(address);
  }
}

void WorkloadCharacterisation::memoryAtomicStore(const Memory *memory, const WorkItem *workItem, AtomicOp op, size_t address, size_t size) {
  if (memory->getAddressSpace() != 0) {
    m_state.memoryOps->push_back(address);
  }
}

void WorkloadCharacterisation::instructionExecuted(
    const WorkItem *workItem, const llvm::Instruction *instruction,
    const TypedValue &result) {

  unsigned opcode = instruction->getOpcode();
  std::string opcode_name = llvm::Instruction::getOpcodeName(opcode);
  (*m_state.computeOps)[opcode_name]++;

  //cout << ">> Instruction " << instruction->getOpcodeName() << " type " << instruction->getType() << endl;
  //TEST to see if the instruction is using local memory:
  bool isMemoryInst = false;
  unsigned addressSpace;
  if (instruction->getOpcode() == llvm::Instruction::Load) {
    isMemoryInst = true;
    const llvm::StoreInst *storeInst = (const llvm::StoreInst *)instruction;
    addressSpace = storeInst->getPointerAddressSpace();
  } else if (instruction->getOpcode() == llvm::Instruction::Store) {
    isMemoryInst = true;
    const llvm::LoadInst *loadInst = (const llvm::LoadInst *)instruction;
    addressSpace = loadInst->getPointerAddressSpace();
  }
  if (isMemoryInst) {
    //cout << ">> Instruction " << instruction->getOpcodeName() << " address space " << addressSpace << endl;
    switch (addressSpace) {
    case AddrSpaceLocal: {
      m_state.local_memory_access_count++;
      break;
    }
    case AddrSpaceGlobal: {
      m_state.global_memory_access_count++;
      break;
    }
    case AddrSpaceConstant: {
      m_state.constant_memory_access_count++;
      break;
    }
    case AddrSpacePrivate:
    default: {
      // we don't count these
    }
    }
  }

  //get all unique labels -- for register use -- and the # of instructions between loads and stores -- as the freedom to reorder
  m_state.ops_between_load_or_store++;
  if (auto inst = llvm::dyn_cast<llvm::LoadInst>(instruction)) {
    std::string name = inst->getPointerOperand()->getName().data();
    (*m_state.loadInstructionLabels)[name]++;
    m_state.instructionsBetweenLoadOrStore->push_back(m_state.ops_between_load_or_store);
    m_state.ops_between_load_or_store = 0;
  } else if (auto inst = llvm::dyn_cast<llvm::StoreInst>(instruction)) {
    std::string name = inst->getPointerOperand()->getName().data();
    (*m_state.storeInstructionLabels)[name]++;
    m_state.instructionsBetweenLoadOrStore->push_back(m_state.ops_between_load_or_store);
    m_state.ops_between_load_or_store = 0;
  }

  //collect conditional branches and the associated trace to count which ones were taken and which weren't
  if (m_state.previous_instruction_is_branch == true) {
    std::string Str;
    llvm::raw_string_ostream OS(Str);
    instruction->getParent()->printAsOperand(OS, false);
    OS.flush();
    if (Str == m_state.target1)
      (*m_state.branchOps)[m_state.branch_loc].push_back(true); //taken
    else if (Str == m_state.target2) {
      (*m_state.branchOps)[m_state.branch_loc].push_back(false); //not taken
    } else {
      cout << "Error in branching!" << endl;
      cout << "Str was: " << Str << " but target was either: " << m_state.target1 << " or: " << m_state.target2 << endl;
      std::raise(SIGINT);
    }
    m_state.previous_instruction_is_branch = false;
  }
  //if a conditional branch -- they have labels and thus temporarily store them and see where we jump to in the next instruction
  if (opcode == llvm::Instruction::Br && instruction->getNumOperands() == 3) {
    if (instruction->getOperand(1)->getType()->isLabelTy() && instruction->getOperand(2)->getType()->isLabelTy()) {
      m_state.previous_instruction_is_branch = true;
      std::string Str;
      llvm::raw_string_ostream OS(Str);
      instruction->getOperand(1)->printAsOperand(OS, false);
      OS.flush();
      m_state.target1 = Str;
      Str = "";
      instruction->getOperand(2)->printAsOperand(OS, false);
      OS.flush();
      m_state.target2 = Str;
      llvm::DebugLoc loc = instruction->getDebugLoc();
      m_state.branch_loc = loc.getLine();
    }
  }

  //counter for instructions to barrier and other parallelism metrics
  m_state.instruction_count++;
  m_state.workitem_instruction_count++;

  //SIMD instruction width metrics use the following
  (*m_state.instructionWidth)[result.num]++;

  //TODO: add support for Phi, Switch and Select control operations
}

void WorkloadCharacterisation::workItemBarrier(const WorkItem *workItem) {
  m_state.barriers_hit++;
  m_state.instructionsBetweenBarriers->push_back(m_state.instruction_count);
  m_state.instruction_count = 0;
}

void WorkloadCharacterisation::workItemClearBarrier(const WorkItem *workItem) {
  m_state.instruction_count = 0;
}

void WorkloadCharacterisation::workItemBegin(const WorkItem *workItem) {
  m_state.threads_invoked++;
  m_state.instruction_count = 0;
  m_state.workitem_instruction_count = 0;
  m_state.ops_between_load_or_store = 0;
}

void WorkloadCharacterisation::workItemComplete(const WorkItem *workItem) {
  m_state.instructionsBetweenBarriers->push_back(m_state.instruction_count);
  m_state.instructionsPerWorkitem->push_back(m_state.workitem_instruction_count);
}

void WorkloadCharacterisation::kernelBegin(const KernelInvocation *kernelInvocation) {
  //update the list of memory copies from host to device; since the only reason to write to the device is before an execution.
  m_last_kernel_name = kernelInvocation->getKernel()->getName();

  int end_of_list = m_hostToDeviceCopy.size() - 1;
  for (int i = 0; i < m_numberOfHostToDeviceCopiesBeforeKernelNamed; i++) {
    m_hostToDeviceCopy[end_of_list - i] = m_last_kernel_name;
  }
  m_numberOfHostToDeviceCopiesBeforeKernelNamed = 0;

  m_memoryOps.clear();
  m_computeOps.clear();
  m_branchOps.clear();
  m_instructionsToBarrier.clear();
  m_instructionWidth.clear();
  m_instructionsPerWorkitem.clear();
  m_instructionsBetweenLoadOrStore.clear();
  m_loadInstructionLabels.clear();
  m_storeInstructionLabels.clear();
  m_threads_invoked = 0;
  m_barriers_hit = 0;
  m_global_memory_access = 0;
  m_local_memory_access = 0;
  m_constant_memory_access = 0;
}

void WorkloadCharacterisation::kernelEnd(const KernelInvocation *kernelInvocation) {
  // Load default locale
  locale previousLocale = cout.getloc();
  locale defaultLocale("");
  cout.imbue(defaultLocale);

  cout << endl
       << "# Architecture-Independent Workload Characterization of kernel: " << kernelInvocation->getKernel()->getName() << endl;

  cout << endl
       << "## Compute" << endl
       << endl;

  std::vector<std::pair<std::string, size_t>> sorted_ops(m_computeOps.size());
  std::partial_sort_copy(m_computeOps.begin(), m_computeOps.end(), sorted_ops.begin(), sorted_ops.end(), [](const std::pair<std::string, size_t> &left, const std::pair<std::string, size_t> &right) {
    return (left.second > right.second);
  });

  cout << "|" << setw(20) << left << "Opcode"
       << "|" << setw(12) << right << "count"
       << "|" << endl;
  cout << "|--------------------|-----------:|" << endl;
  for (auto const &item : sorted_ops)
    cout << "|" << setw(20) << left << item.first << "|" << setw(12) << right << item.second << "|" << endl;
  cout << endl;

  size_t operation_count = 0;
  for (auto const &item : sorted_ops)
    operation_count += item.second;
  size_t significant_operation_count = (unsigned)ceil(operation_count * 0.9);
  size_t major_operations = 0;
  size_t total_instruction_count = operation_count;
  operation_count = 0;

  cout << "unique opcodes required to cover 90\% of dynamic instructions: ";
  while (operation_count < significant_operation_count) {
    if (major_operations > 0)
      cout << ", ";
    operation_count += sorted_ops[major_operations].second;
    cout << sorted_ops[major_operations].first;
    major_operations++;
  }
  cout << endl
       << endl;

  cout << "num unique opcodes required to cover 90\% of dynamic instructions: " << major_operations << endl
       << endl;
  cout << "Total Instruction Count: " << total_instruction_count << endl;

  cout << endl
       << "## Parallelism" << endl;

  cout << endl
       << "### Utilization" << endl
       << endl;

  double freedom_to_reorder = std::accumulate(m_instructionsBetweenLoadOrStore.begin(), m_instructionsBetweenLoadOrStore.end(), 0.0);
  freedom_to_reorder = freedom_to_reorder / m_instructionsBetweenLoadOrStore.size();
  cout << "Freedom to Reorder: " << setprecision(2) << freedom_to_reorder << endl
       << endl;

  double resource_pressure = 0;
  for (auto const &item : m_storeInstructionLabels)
    resource_pressure += item.second;
  for (auto const &item : m_loadInstructionLabels)
    resource_pressure += item.second;
  resource_pressure = resource_pressure / m_threads_invoked;
  cout << "Resource Pressure: " << setprecision(2) << resource_pressure << endl;

  cout << endl
       << "### Thread-Level Parallelism" << endl
       << endl;

  cout << "Work-items: " << m_threads_invoked << endl
       << endl;
  double granularity = 1.0 / static_cast<double>(m_threads_invoked);
  cout << "Granularity: " << granularity << endl
       << endl;

  cout << "Total Barriers Hit: " << m_barriers_hit << endl
       << endl;
  //cout << "num barriers hit per thread: " << m_instructionsToBarrier.size()/m_threads_invoked << endl;
  unsigned int itb_min = *std::min_element(m_instructionsToBarrier.begin(), m_instructionsToBarrier.end());
  unsigned int itb_max = *std::max_element(m_instructionsToBarrier.begin(), m_instructionsToBarrier.end());

  double itb_median;
  std::vector<unsigned int> itb = m_instructionsToBarrier;
  sort(itb.begin(), itb.end());

  size_t size = itb.size();
  if (size % 2 == 0) {
    itb_median = (itb[size / 2 - 1] + itb[size / 2]) / 2;
  } else {
    itb_median = itb[size / 2];
  }
  cout << "Instructions to Barrier (min/max/median): " << itb_min << "/" << itb_max << "/" << itb_median << endl
       << endl;
  double barriers_per_instruction = static_cast<double>(m_barriers_hit + m_threads_invoked) / static_cast<double>(total_instruction_count);
  cout << "Barriers per Instruction: " << barriers_per_instruction << endl
       << endl;

  cout << "### Work Distribution" << endl
       << endl;

  unsigned int ipt_min = *std::min_element(m_instructionsPerWorkitem.begin(), m_instructionsPerWorkitem.end());
  unsigned int ipt_max = *std::max_element(m_instructionsPerWorkitem.begin(), m_instructionsPerWorkitem.end());

  unsigned int ipt_median;
  std::vector<unsigned int> ipt = m_instructionsPerWorkitem;
  sort(ipt.begin(), ipt.end());

  size = ipt.size();
  if (size % 2 == 0) {
    ipt_median = (ipt[size / 2 - 1] + ipt[size / 2]) / 2;
  } else {
    ipt_median = ipt[size / 2];
  }
  cout << "Instructions per Thread (min/max/median): " << ipt_min << "/" << ipt_max << "/" << ipt_median << endl
       << endl;

  cout << "### Data Parallelism" << endl
       << endl;

  using pair_type = decltype(m_instructionWidth)::value_type;

  unsigned simd_min = std::min_element(m_instructionWidth.begin(), m_instructionWidth.end(), [](const pair_type &a, const pair_type &b) { return a.first < b.first; })->first;
  unsigned simd_max = std::max_element(m_instructionWidth.begin(), m_instructionWidth.end(), [](const pair_type &a, const pair_type &b) { return a.first < b.first; })->first;

  long simd_sum = 0;
  long simd_num = 0;
  for (const auto &item : m_instructionWidth) {
    simd_sum += item.second * item.first;
    simd_num += item.second;
  }
  double simd_mean = simd_sum / (double)simd_num;
  std::vector<double> diff(m_instructionWidth.size());
  std::transform(m_instructionWidth.begin(), m_instructionWidth.end(), diff.begin(), [simd_mean](const pair_type &x) { return (x.first - simd_mean) * (x.first - simd_mean) * x.second; });
  double simd_sq_sum = std::accumulate(diff.begin(), diff.end(), 0.0);
  double simd_stdev = std::sqrt(simd_sq_sum / (double)simd_num);
  cout << "SIMD Width (min/max/mean/stdev): " << simd_min << "/" << simd_max << "/" << simd_mean << "/" << simd_stdev << endl
       << endl;

  double instructions_per_operand = static_cast<double>(total_instruction_count) / simd_sum;
  cout << "Instructions per Operand: " << instructions_per_operand << endl
       << endl;

  cout << "## Memory" << endl
       << endl;

  cout << "### Memory Footprint" << endl
       << endl;

  // count accesses to memory addresses with different numbers of retained
  // significant bits
  std::vector<std::unordered_map<size_t, unsigned>> local_address_count(11);
  for (const auto &address : m_memoryOps) {
    for (int nskip = 0; nskip <= 10; nskip++) {
      size_t local_addr = address >> nskip;
      local_address_count[nskip][local_addr]++;
    }
  }

  std::vector<std::pair<unsigned, size_t>> sorted_count(local_address_count[0].size());
  std::partial_sort_copy(local_address_count[0].begin(), local_address_count[0].end(), sorted_count.begin(), sorted_count.end(), [](const std::pair<unsigned, size_t> &left, const std::pair<unsigned, size_t> &right) {
    return (left.second > right.second);
  });

  size_t memory_access_count = 0;
  for (auto const &e : sorted_count) {
    memory_access_count += e.second;
    //cout << "address: "<< e.first << " accessed: " << e.second << " times!" << endl;
  }

  cout << "num memory accesses: " << memory_access_count << endl
       << endl;
  cout << "Total Memory Footprint -- num unique memory addresses accessed: " << local_address_count[0].size() << endl
       << endl;
  size_t significant_memory_access_count = (unsigned)ceil(memory_access_count * 0.9);
  cout << "90\% of memory accesses: " << significant_memory_access_count << endl
       << endl;

  size_t unique_memory_addresses = 0;
  size_t access_count = 0;
  while (access_count < significant_memory_access_count) {
    access_count += sorted_count[unique_memory_addresses].second;
    unique_memory_addresses++;
  }
  cout << "90% Memory Footprint -- num unique memory addresses that cover 90\% of memory accesses: " << unique_memory_addresses << endl
       << endl;
  //cout << "the top 10:" << endl;
  //for (int i = 0; i < 10; i++){
  //    cout << "address: " << sorted_count[i].first << " contributed: " << sorted_count[i].second << " accesses!" << endl;
  //}

  cout << "### Memory Entropy" << endl
       << endl;

  double mem_entropy = 0.0;
  for (const auto &it : sorted_count) {
    int value = (int)it.second;
    double prob = (double)value * 1.0 / (double)memory_access_count;
    mem_entropy = mem_entropy - prob * std::log2(prob);
  }
  cout << "Global Memory Address Entropy -- measure of the randomness of memory addresses: " << (float)mem_entropy << endl
       << endl;

  cout << "Local Memory Address Entropy -- measure of the spatial locality of memory addresses" << endl
       << endl;

  cout << "|" << setw(12) << right << "LSBs skipped"
       << "|" << setw(8) << right << "Entropy"
       << "|" << endl;
  cout << "|-----------:|-------:|" << endl;
  std::vector<float> loc_entropy;
  for (int nskip = 1; nskip < 11; nskip++) {
    double local_entropy = 0.0;
    for (const auto &it : local_address_count[nskip]) {
      double prob = (double)(it.second) * 1.0 / (double)memory_access_count;
      local_entropy = local_entropy - prob * std::log2(prob);
    }
    loc_entropy.push_back((float)local_entropy);
    cout << "|" << setw(12) << right << nskip << "|" << fixed << setw(8) << setprecision(4) << right << (float)local_entropy << "|" << endl;
  }

  cout << endl
       << "### Memory Diversity -- Usage of local and constant memory relative to global memory" << endl
       << endl;

  cout << "num global memory accesses: " << m_global_memory_access << endl
       << endl;
  cout << "num local memory accesses: " << m_local_memory_access << endl
       << endl;
  cout << "num constant memory accesses: " << m_constant_memory_access << endl
       << endl;

  unsigned m_total_memory_access = m_global_memory_access + m_local_memory_access + m_constant_memory_access;

  cout << "\% local memory accesses (local/total): " << setprecision(2) << (((float)m_local_memory_access / (float)m_total_memory_access) * 100) << endl
       << endl;
  cout << "\% constant memory accesses (constant/total): " << setprecision(2) << (((float)m_constant_memory_access / (float)m_total_memory_access) * 100) << endl
       << endl;

  cout << "## Control" << endl
       << endl;

  cout << "Unique Branch Instructions -- Total number of unique branch instructions to cover 90\% of the branches" << endl
       << endl;

  std::vector<std::pair<unsigned, std::vector<bool>>> sorted_branch_ops(m_branchOps.size());
  std::partial_sort_copy(m_branchOps.begin(), m_branchOps.end(), sorted_branch_ops.begin(), sorted_branch_ops.end(), [](const std::pair<unsigned, std::vector<bool>> &left, const std::pair<unsigned, std::vector<bool>> &right) {
    return (left.second.size() > right.second.size());
  });

  cout << "|" << setw(14) << left << "Branch At Line"
       << "|" << setw(23) << right << "Count (hit and miss)"
       << "|" << endl;
  cout << "|--------------|----------------------:|" << endl;
  size_t branch_op_count = 0;
  for (auto const &x : sorted_branch_ops) {
    branch_op_count += x.second.size();
    cout << "|" << setw(14) << left << x.first << "|" << setw(23) << right << x.second.size() << "|" << endl;
  }
  cout << endl;

  size_t significant_branch_op_count = (unsigned)ceil(branch_op_count * 0.9);

  size_t unique_branch_addresses = 0;
  size_t branch_count = 0;
  while (branch_count < significant_branch_op_count) {
    branch_count += sorted_branch_ops[unique_branch_addresses].second.size();
    unique_branch_addresses++;
  }
  cout << "Number of unique branches that cover 90\% of all branch instructions: " << unique_branch_addresses << endl;

  cout << endl
       << "### Branch Entropy -- measure of the randomness of branch behaviour, representing branch predictability" << endl
       << endl;

  //generate a history pattern
  //(arbitarily selected to a history of m=16 branches?)
  const unsigned m = 16;
  float average_entropy = 0.0f;
  float yokota_entropy = 0.0f;
  float yokota_entropy_per_workload = 0.0f;
  unsigned N = 0;

  for (auto const &branch : m_branchOps) {
    //if we have fewer branches than the history window, skip it.
    if (branch.second.size() < m)
      continue;

    //generate the set of history patterns
    std::unordered_map<std::string, unsigned> H;
    for (unsigned i = 0; i < branch.second.size() - (m - 1); i++) {
      std::string current_pattern = "";
      for (unsigned j = 0; j < m; j++) {
        bool b = branch.second[i + j];
        current_pattern += b ? '1' : '0';
      }
      H[current_pattern]++;
    }

    for (auto const &h : H) {
      std::string pattern = h.first;
      unsigned number_of_occurances = h.second;
      //for each history pattern compute the probability of the taken branch
      unsigned taken = std::count(pattern.begin(), pattern.end(), '1');
      unsigned not_taken = std::count(pattern.begin(), pattern.end(), '0');
      float probability_of_taken = (float)taken / (float)(not_taken + taken);

      //compute Yokota branch entropy
      if (probability_of_taken != 0) {
        yokota_entropy -= number_of_occurances * probability_of_taken * std::log2(probability_of_taken);
        yokota_entropy_per_workload -= probability_of_taken * std::log2(probability_of_taken);
      }
      //compute linear branch entropy
      float linear_branch_entropy = 2 * std::min(probability_of_taken, 1 - probability_of_taken);
      average_entropy += number_of_occurances * linear_branch_entropy;
      N += number_of_occurances;
    }
  }
  average_entropy = average_entropy / N;
  if (isnan(average_entropy)) {
    average_entropy = 0.0;
  }
  cout << "Using a branch history of " << m << endl
       << endl;
  cout << "Yokota Branch Entropy: " << yokota_entropy << endl
       << endl;
  cout << "Yokota Branch Entropy per Workload: " << yokota_entropy_per_workload << endl
       << endl;
  cout << "Average Linear Branch Entropy: " << average_entropy << endl
       << endl;

  int logfile_count = 0;
  std::string logfile_name = "aiwc_" + kernelInvocation->getKernel()->getName() + "_" + std::to_string(logfile_count) + ".csv";
  while (std::ifstream(logfile_name)) {
    logfile_count++;
    logfile_name = "aiwc_" + kernelInvocation->getKernel()->getName() + "_" + std::to_string(logfile_count) + ".csv";
  }
  std::ofstream logfile;
  logfile.open(logfile_name);
  assert(logfile);
  logfile << "metric,count\n";
  logfile << "opcode," << major_operations << "\n";
  logfile << "total instruction count," << total_instruction_count << "\n";
  logfile << "freedom to reorder," << freedom_to_reorder << "\n";
  logfile << "resource pressure," << resource_pressure << "\n";
  logfile << "workitems," << m_threads_invoked << "\n";
  logfile << "operand sum," << simd_sum << "\n";
  logfile << "total # of barriers hit," << m_barriers_hit << "\n";
  logfile << "min instructions to barrier," << itb_min << "\n";
  logfile << "max instructions to barrier," << itb_max << "\n";
  logfile << "median instructions to barrier," << itb_median << "\n";
  logfile << "min instructions executed by a work-item," << ipt_min << "\n";
  logfile << "max instructions executed by a work-item," << ipt_max << "\n";
  logfile << "median instructions executed by a work-item," << ipt_median << "\n";
  logfile << "max simd width," << std::max_element(m_instructionWidth.begin(), m_instructionWidth.end(), [](const pair_type &a, const pair_type &b) { return a.first < b.first; })->first << "\n";
  logfile << "mean simd width," << simd_mean << "\n";
  logfile << "stdev simd width," << simd_stdev << "\n";
  logfile << "granularity," << granularity << "\n";
  logfile << "barriers per instruction," << barriers_per_instruction << "\n";
  logfile << "instructions per operand," << instructions_per_operand << "\n";
  logfile << "total memory footprint," << local_address_count[0].size() << "\n";
  logfile << "90\% memory footprint," << unique_memory_addresses << "\n";
  logfile << "global memory address entropy," << mem_entropy << "\n";
  for (int nskip = 1; nskip < 11; nskip++) {
    logfile << "local memory address entropy -- " << nskip << " LSBs skipped," << loc_entropy[nskip - 1] << "\n";
  }
  logfile << "total global memory accessed," << m_global_memory_access << "\n";
  logfile << "total local memory accessed," << m_local_memory_access << "\n";
  logfile << "total constant memory accessed," << m_constant_memory_access << "\n";
  logfile << "relative local memory usage," << (((float)m_local_memory_access / (float)m_total_memory_access) * 100) << "\n";
  logfile << "relative constant memory usage," << (((float)m_constant_memory_access / (float)m_total_memory_access) * 100) << "\n";
  logfile << "total unique branch instructions," << sorted_branch_ops.size() << "\n";
  logfile << "90\% branch instructions," << unique_branch_addresses << "\n";
  logfile << "branch entropy (yokota)," << yokota_entropy_per_workload << "\n";
  logfile << "branch entropy (average linear)," << average_entropy << "\n";
  logfile.close();

  cout << endl
       << "The Architecture-Independent Workload Characterisation was written to file: " << logfile_name << endl;
  // Restore locale
  cout.imbue(previousLocale);

  // Reset kernel counts, ready to start anew
  m_memoryOps.clear();
  m_computeOps.clear();
  m_branchOps.clear();
  m_instructionsToBarrier.clear();
  m_instructionsPerWorkitem.clear();
  m_threads_invoked = 0;
  m_instructionsBetweenLoadOrStore.clear();
  m_loadInstructionLabels.clear();
  m_storeInstructionLabels.clear();
}

void WorkloadCharacterisation::workGroupBegin(const WorkGroup *workGroup) {
  // Create worker state if haven't already
  if (!m_state.memoryOps) {
    m_state.memoryOps = new vector<size_t>;
    m_state.computeOps = new unordered_map<std::string, size_t>;
    m_state.branchOps = new unordered_map<unsigned, std::vector<bool>>;
    m_state.instructionsBetweenBarriers = new vector<unsigned>;
    m_state.instructionWidth = new unordered_map<unsigned, size_t>;
    m_state.instructionsPerWorkitem = new vector<unsigned>;
    m_state.instructionsBetweenLoadOrStore = new vector<unsigned>;
    m_state.loadInstructionLabels = new unordered_map<std::string, size_t>;
    m_state.storeInstructionLabels = new unordered_map<std::string, size_t>;
  }

  m_state.memoryOps->clear();
  m_state.computeOps->clear();
  m_state.branchOps->clear();
  m_state.instructionsBetweenBarriers->clear();
  m_state.instructionWidth->clear();
  m_state.instructionsPerWorkitem->clear();
  m_state.instructionsBetweenLoadOrStore->clear();
  m_state.loadInstructionLabels->clear();
  m_state.storeInstructionLabels->clear();

  m_state.threads_invoked = 0;
  m_state.instruction_count = 0;
  m_state.barriers_hit = 0;

  //memory type access count variables
  m_state.constant_memory_access_count = 0;
  m_state.local_memory_access_count = 0;
  m_state.global_memory_access_count = 0;

  //branch logic variables
  m_state.previous_instruction_is_branch = false;
  m_state.target1 = "";
  m_state.target2 = "";
  m_state.branch_loc = 0;
}

void WorkloadCharacterisation::workGroupComplete(const WorkGroup *workGroup) {

  lock_guard<mutex> lock(m_mtx);

  // merge operation counts back into global unordered map
  for (auto const &item : (*m_state.computeOps))
    m_computeOps[item.first] += item.second;

  // merge memory operations into global list
  m_memoryOps.insert(m_memoryOps.end(), m_state.memoryOps->begin(), m_state.memoryOps->end());
  m_state.memoryOps->clear();

  // merge control operations into global unordered map
  for (auto const &item : (*m_state.branchOps))
    for (auto const &branch_taken : item.second)
      m_branchOps[item.first].push_back(branch_taken);

  // add the current work-group item / thread counter to the global variable
  m_threads_invoked += m_state.threads_invoked;

  // add the instructions between barriers back to the global setting
  for (auto const &item : (*m_state.instructionsBetweenBarriers))
    m_instructionsToBarrier.push_back(item);

  m_barriers_hit += m_state.barriers_hit;

  // add the SIMD scores back to the global setting
  for (auto const &item : (*m_state.instructionWidth))
    m_instructionWidth[item.first] += item.second;

  // add the instructions executed per workitem scores back to the global setting
  for (auto const &item : (*m_state.instructionsPerWorkitem))
    m_instructionsPerWorkitem.push_back(item);

  // add the instruction reordering (flexability) metrics
  for (auto const &item : (*m_state.instructionsBetweenLoadOrStore))
    m_instructionsBetweenLoadOrStore.push_back(item);

  for (auto const &item : (*m_state.loadInstructionLabels))
    m_loadInstructionLabels[item.first] += item.second;

  for (auto const &item : (*m_state.storeInstructionLabels))
    m_storeInstructionLabels[item.first] += item.second;

  // merge memory type access count variables
  m_constant_memory_access += m_state.constant_memory_access_count;
  m_local_memory_access += m_state.local_memory_access_count;
  m_global_memory_access += m_state.global_memory_access_count;
}

