use Time, Types, NPBRandom;
use BradsBlock1DPar;

use HPCCProblemSize;


param numVectors = 3;
type elemType = real(64);

config const m = computeProblemSize(elemType, numVectors),
             alpha = 3.0;

config const numTrials = 10,
             epsilon = 0.0;

config const useRandomSeed = true,
             seed = if useRandomSeed then oddTimeSeed() else 314159265;

config const printParams = true,
             printArrays = false,
             printStats = true;


proc main() {
  printConfiguration();

  const ProblemDist = new unmanaged Block1DDist(bbox={1..m}, targetLocales=Locales);

  const ProblemSpace = ProblemDist.newDomain({1..m}, int(64));

  var A = ProblemSpace.newArray(elemType), 
      B = ProblemSpace.newArray(elemType),
      C = ProblemSpace.newArray(elemType);

  initVectors(B, C);

  var execTime: [1..numTrials] real;

  for trial in 1..numTrials {
    const startTime = timeSinceEpoch().totalSeconds();
    // TODO: Ultimately, Want:
    //
    //   A = B + alpha * C;
    //
    // But for now we'll do a manual invocation of leader/follower following
    // Steve's proposal -- This version leads and follows over arrays:

    coforall block in A.newThese(IteratorType.leader) {
      on block {
        if debugBradsBlock1D then
          writeln("locale ", here, " is being asked to follow ", block);
        for (a, b, c) in zip(A.newThese(IteratorType.follower, block),
                          B.newThese(IteratorType.follower, block),
                          C.newThese(IteratorType.follower, block)) {
          a = b + alpha * c;
        }
      }
    }
          
    execTime(trial) = timeSinceEpoch().totalSeconds() - startTime;
  }

  const validAnswer = verifyResults(A, B, C);
  printResults(validAnswer, execTime);
  delete A;
  delete B;
  delete C;
  delete ProblemSpace;
  delete ProblemDist;
}


proc printConfiguration() {
  if (printParams) {
    printProblemSize(elemType, numVectors, m);
    writeln("Number of trials = ", numTrials, "\n");
  }
}


proc initVectors(B, C) {
  // TODO: should write a fillRandom() implementation that does this
  coforall loc in B.dom.dist.targetLocDom {
    on B.dom.dist.targetLocs(loc) {
      var randlist = new owned NPBRandomStream(eltType=real, seed=seed);
      // TODO: Need to clean this up to use more normal method names
      randlist.skipToNth(B.locArr(loc)!.locDom.low-1);
      randlist.fillRandom(B.locArr(loc)!.myElems);
      randlist.skipToNth(B.size + C.locArr(loc)!.locDom.low-1);
      randlist.fillRandom(C.locArr(loc)!.myElems);
    }
  }

  if (printArrays) {
    writeln("B is: ", B, "\n");
    writeln("C is: ", C, "\n");
  }
}


proc verifyResults(A, B, C) {
  if (printArrays) then writeln("A is: ", A, "\n");

  const infNorm = max reduce [i in A.dom] abs(A(i) - (B(i) + alpha * C(i)));

  return (infNorm <= epsilon);
}


proc printResults(successful, execTimes) {
  writeln("Validation: ", if successful then "SUCCESS" else "FAILURE");
  if (printStats) {
    const totalTime = + reduce execTimes,
          avgTime = totalTime / numTrials,
          minTime = min reduce execTimes;
    writeln("Execution time:");
    writeln("  tot = ", totalTime);
    writeln("  avg = ", avgTime);
    writeln("  min = ", minTime);

    const GBPerSec = numVectors * numBytes(elemType) * (m / minTime) * 1e-9;
    writeln("Performance (GB/s) = ", GBPerSec);
  }
}

// TODO: Check diff with main stream.chpl to see what differs
