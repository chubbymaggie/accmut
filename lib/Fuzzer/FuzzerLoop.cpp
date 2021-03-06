//===- FuzzerLoop.cpp - Fuzzer's main loop --------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// Fuzzer's main loop.
//===----------------------------------------------------------------------===//

#include "FuzzerInternal.h"
#include <sanitizer/coverage_interface.h>
#include <algorithm>

namespace fuzzer {
static const size_t kMaxUnitSizeToPrint = 4096;

// Only one Fuzzer per process.
static Fuzzer *F;

Fuzzer::Fuzzer(UserSuppliedFuzzer &USF, FuzzingOptions Options)
    : USF(USF), Options(Options) {
  SetDeathCallback();
  InitializeTraceState();
  assert(!F);
  F = this;
}

void Fuzzer::SetDeathCallback() {
  __sanitizer_set_death_callback(StaticDeathCallback);
}

void Fuzzer::PrintUnitInASCIIOrTokens(const Unit &U, const char *PrintAfter) {
  if (Options.Tokens.empty()) {
    PrintASCII(U, PrintAfter);
  } else {
    auto T = SubstituteTokens(U);
    T.push_back(0);
    Printf("%s%s", T.data(), PrintAfter);
  }
}

void Fuzzer::StaticDeathCallback() {
  assert(F);
  F->DeathCallback();
}

void Fuzzer::DeathCallback() {
  Printf("DEATH:\n");
  Print(CurrentUnit, "\n");
  PrintUnitInASCIIOrTokens(CurrentUnit, "\n");
  WriteUnitToFileWithPrefix(CurrentUnit, "crash-");
}

void Fuzzer::StaticAlarmCallback() {
  assert(F);
  F->AlarmCallback();
}

void Fuzzer::AlarmCallback() {
  assert(Options.UnitTimeoutSec > 0);
  size_t Seconds =
      duration_cast<seconds>(system_clock::now() - UnitStartTime).count();
  if (Seconds == 0) return;
  if (Options.Verbosity >= 2)
    Printf("AlarmCallback %zd\n", Seconds);
  if (Seconds >= (size_t)Options.UnitTimeoutSec) {
    Printf("ALARM: working on the last Unit for %zd seconds\n", Seconds);
    Printf("       and the timeout value is %d (use -timeout=N to change)\n",
           Options.UnitTimeoutSec);
    if (CurrentUnit.size() <= kMaxUnitSizeToPrint)
      Print(CurrentUnit, "\n");
    PrintUnitInASCIIOrTokens(CurrentUnit, "\n");
    WriteUnitToFileWithPrefix(CurrentUnit, "timeout-");
    exit(1);
  }
}

void Fuzzer::PrintStats(const char *Where, size_t Cov, const char *End) {
  if (!Options.Verbosity) return;
  size_t Seconds = secondsSinceProcessStartUp();
  size_t ExecPerSec = (Seconds ? TotalNumberOfRuns / Seconds : 0);
  Printf("#%zd\t%s cov: %zd bits: %zd units: %zd exec/s: %zd",
         TotalNumberOfRuns, Where, Cov, TotalBits(), Corpus.size(), ExecPerSec);
  if (TotalNumberOfExecutedTraceBasedMutations)
    Printf(" tbm: %zd", TotalNumberOfExecutedTraceBasedMutations);
  Printf("%s", End);
}

void Fuzzer::RereadOutputCorpus() {
  if (Options.OutputCorpus.empty()) return;
  std::vector<Unit> AdditionalCorpus;
  ReadDirToVectorOfUnits(Options.OutputCorpus.c_str(), &AdditionalCorpus,
                         &EpochOfLastReadOfOutputCorpus);
  if (Corpus.empty()) {
    Corpus = AdditionalCorpus;
    return;
  }
  if (!Options.Reload) return;
  if (Options.Verbosity >= 2)
    Printf("Reload: read %zd new units.\n",  AdditionalCorpus.size());
  for (auto &X : AdditionalCorpus) {
    if (X.size() > (size_t)Options.MaxLen)
      X.resize(Options.MaxLen);
    if (UnitHashesAddedToCorpus.insert(Hash(X)).second) {
      CurrentUnit.clear();
      CurrentUnit.insert(CurrentUnit.begin(), X.begin(), X.end());
      size_t NewCoverage = RunOne(CurrentUnit);
      if (NewCoverage) {
        Corpus.push_back(X);
        if (Options.Verbosity >= 1)
          PrintStats("RELOAD", NewCoverage);
      }
    }
  }
}

void Fuzzer::ShuffleAndMinimize() {
  size_t MaxCov = 0;
  bool PreferSmall = (Options.PreferSmallDuringInitialShuffle == 1 ||
                      (Options.PreferSmallDuringInitialShuffle == -1 &&
                       USF.GetRand().RandBool()));
  if (Options.Verbosity)
    Printf("PreferSmall: %d\n", PreferSmall);
  PrintStats("READ  ", 0);
  std::vector<Unit> NewCorpus;
  std::random_shuffle(Corpus.begin(), Corpus.end(), USF.GetRand());
  if (PreferSmall)
    std::stable_sort(
        Corpus.begin(), Corpus.end(),
        [](const Unit &A, const Unit &B) { return A.size() < B.size(); });
  Unit &U = CurrentUnit;
  for (const auto &C : Corpus) {
    for (size_t First = 0; First < 1; First++) {
      U.clear();
      size_t Last = std::min(First + Options.MaxLen, C.size());
      U.insert(U.begin(), C.begin() + First, C.begin() + Last);
      if (Options.OnlyASCII)
        ToASCII(U);
      size_t NewCoverage = RunOne(U);
      if (NewCoverage) {
        MaxCov = NewCoverage;
        NewCorpus.push_back(U);
        if (Options.Verbosity >= 2)
          Printf("NEW0: %zd L %zd\n", NewCoverage, U.size());
      }
    }
  }
  Corpus = NewCorpus;
  for (auto &X : Corpus)
    UnitHashesAddedToCorpus.insert(Hash(X));
  PrintStats("INITED", MaxCov);
}

size_t Fuzzer::RunOne(const Unit &U) {
  UnitStartTime = system_clock::now();
  TotalNumberOfRuns++;
  size_t Res = RunOneMaximizeTotalCoverage(U);
  auto UnitStopTime = system_clock::now();
  auto TimeOfUnit =
      duration_cast<seconds>(UnitStopTime - UnitStartTime).count();
  if (TimeOfUnit > TimeOfLongestUnitInSeconds &&
      TimeOfUnit >= Options.ReportSlowUnits) {
    TimeOfLongestUnitInSeconds = TimeOfUnit;
    Printf("Slowest unit: %zd s:\n", TimeOfLongestUnitInSeconds);
    if (U.size() <= kMaxUnitSizeToPrint)
      Print(U, "\n");
    WriteUnitToFileWithPrefix(U, "slow-unit-");
  }
  return Res;
}

void Fuzzer::RunOneAndUpdateCorpus(Unit &U) {
  if (TotalNumberOfRuns >= Options.MaxNumberOfRuns)
    return;
  if (Options.OnlyASCII)
    ToASCII(U);
  ReportNewCoverage(RunOne(U), U);
}

Unit Fuzzer::SubstituteTokens(const Unit &U) const {
  Unit Res;
  for (auto Idx : U) {
    if (Idx < Options.Tokens.size()) {
      std::string Token = Options.Tokens[Idx];
      Res.insert(Res.end(), Token.begin(), Token.end());
    } else {
      Res.push_back(' ');
    }
  }
  // FIXME: Apply DFSan labels.
  return Res;
}

void Fuzzer::ExecuteCallback(const Unit &U) {
  int Res = 0;
  if (Options.Tokens.empty()) {
    Res = USF.TargetFunction(U.data(), U.size());
  } else {
    auto T = SubstituteTokens(U);
    Res = USF.TargetFunction(T.data(), T.size());
  }
  assert(Res == 0);
}

size_t Fuzzer::RunOneMaximizeTotalCoverage(const Unit &U) {
  size_t NumCounters = __sanitizer_get_number_of_counters();
  if (Options.UseCounters) {
    CounterBitmap.resize(NumCounters);
    __sanitizer_update_counter_bitset_and_clear_counters(0);
  }
  size_t OldCoverage = __sanitizer_get_total_unique_coverage();
  ExecuteCallback(U);
  size_t NewCoverage = __sanitizer_get_total_unique_coverage();
  size_t NumNewBits = 0;
  if (Options.UseCounters)
    NumNewBits = __sanitizer_update_counter_bitset_and_clear_counters(
        CounterBitmap.data());

  if (!(TotalNumberOfRuns & (TotalNumberOfRuns - 1)) && Options.Verbosity)
    PrintStats("pulse ", NewCoverage);

  if (NewCoverage > OldCoverage || NumNewBits)
    return NewCoverage;
  return 0;
}

void Fuzzer::WriteToOutputCorpus(const Unit &U) {
  if (Options.OutputCorpus.empty()) return;
  std::string Path = DirPlusFile(Options.OutputCorpus, Hash(U));
  WriteToFile(U, Path);
  if (Options.Verbosity >= 2)
    Printf("Written to %s\n", Path.c_str());
  assert(!Options.OnlyASCII || IsASCII(U));
}

void Fuzzer::WriteUnitToFileWithPrefix(const Unit &U, const char *Prefix) {
  std::string Path = Prefix + Hash(U);
  WriteToFile(U, Path);
  Printf("Test unit written to %s\n", Path.c_str());
  if (U.size() <= kMaxUnitSizeToPrint) {
    Printf("Base64: ");
    PrintFileAsBase64(Path);
  }
}

void Fuzzer::SaveCorpus() {
  if (Options.OutputCorpus.empty()) return;
  for (const auto &U : Corpus)
    WriteToFile(U, DirPlusFile(Options.OutputCorpus, Hash(U)));
  if (Options.Verbosity)
    Printf("Written corpus of %zd files to %s\n", Corpus.size(),
           Options.OutputCorpus.c_str());
}

void Fuzzer::ReportNewCoverage(size_t NewCoverage, const Unit &U) {
  if (!NewCoverage) return;
  Corpus.push_back(U);
  UnitHashesAddedToCorpus.insert(Hash(U));
  PrintStats("NEW   ", NewCoverage, "");
  if (Options.Verbosity) {
    Printf(" L: %zd", U.size());
    if (U.size() < 30) {
      Printf(" ");
      PrintUnitInASCIIOrTokens(U, "\t");
      Print(U);
    }
    Printf("\n");
  }
  WriteToOutputCorpus(U);
  if (Options.ExitOnFirst)
    exit(0);
}

void Fuzzer::MutateAndTestOne(Unit *U) {
  for (int i = 0; i < Options.MutateDepth; i++) {
    StartTraceRecording();
    size_t Size = U->size();
    U->resize(Options.MaxLen);
    size_t NewSize = USF.Mutate(U->data(), Size, U->size());
    assert(NewSize > 0 && "Mutator returned empty unit");
    assert(NewSize <= (size_t)Options.MaxLen &&
           "Mutator return overisized unit");
    U->resize(NewSize);
    RunOneAndUpdateCorpus(*U);
    size_t NumTraceBasedMutations = StopTraceRecording();
    size_t TBMWidth =
        std::min((size_t)Options.TBMWidth, NumTraceBasedMutations);
    size_t TBMDepth =
        std::min((size_t)Options.TBMDepth, NumTraceBasedMutations);
    Unit BackUp = *U;
    for (size_t w = 0; w < TBMWidth; w++) {
      *U = BackUp;
      for (size_t d = 0; d < TBMDepth; d++) {
        TotalNumberOfExecutedTraceBasedMutations++;
        ApplyTraceBasedMutation(USF.GetRand()(NumTraceBasedMutations), U);
        RunOneAndUpdateCorpus(*U);
      }
    }
  }
}

void Fuzzer::Loop() {
  for (auto &U: Options.Dictionary)
    USF.GetMD().AddWordToDictionary(U.data(), U.size());

  while (true) {
    for (size_t J1 = 0; J1 < Corpus.size(); J1++) {
      SyncCorpus();
      RereadOutputCorpus();
      if (TotalNumberOfRuns >= Options.MaxNumberOfRuns)
        return;
      if (Options.MaxTotalTimeSec > 0 &&
          secondsSinceProcessStartUp() >
              static_cast<size_t>(Options.MaxTotalTimeSec))
        return;
      CurrentUnit = Corpus[J1];
      // Optionally, cross with another unit.
      if (Options.DoCrossOver && USF.GetRand().RandBool()) {
        size_t J2 = USF.GetRand()(Corpus.size());
        if (!Corpus[J1].empty() && !Corpus[J2].empty()) {
          assert(!Corpus[J2].empty());
          CurrentUnit.resize(Options.MaxLen);
          size_t NewSize = USF.CrossOver(
              Corpus[J1].data(), Corpus[J1].size(), Corpus[J2].data(),
              Corpus[J2].size(), CurrentUnit.data(), CurrentUnit.size());
          assert(NewSize > 0 && "CrossOver returned empty unit");
          assert(NewSize <= (size_t)Options.MaxLen &&
                 "CrossOver returned overisized unit");
          CurrentUnit.resize(NewSize);
        }
      }
      // Perform several mutations and runs.
      MutateAndTestOne(&CurrentUnit);
    }
  }
}

void Fuzzer::SyncCorpus() {
  if (Options.SyncCommand.empty() || Options.OutputCorpus.empty()) return;
  auto Now = system_clock::now();
  if (duration_cast<seconds>(Now - LastExternalSync).count() <
      Options.SyncTimeout)
    return;
  LastExternalSync = Now;
  ExecuteCommand(Options.SyncCommand + " " + Options.OutputCorpus);
}

}  // namespace fuzzer
