//use CyclicZipOpt;
use BlockDist;
use CyclicDist;
use Time;
use CommDiagnostics;

/****************************
    Dimensions are set up to be M x N. 
    M: 1st dimension
        Default = 32
  N: 2nd dimension 
    Default = 32

  printData: Set to false if you don't want to see the data printed
    Default = false
    dist: the distribution of the domain which the matrices are based on. 
        Default: cyclical with modulo unrolling
*****************************/
config var correct = false;
config var timeit = false;
config var messages = false;
config var M: int = 32;
config var N: int = 32;

config var printData: bool = false;
config var dist: string = "C";

var alpha: int = 32412;
var beta: int = 2123;

/* Initializes a 2D structure */
proc initialize_2D(distribution, m_dim: int) {
    var matrix: [distribution] real = 0.0;
    forall (i,j) in distribution with (ref matrix) {
        matrix[i,j] = ((i - 1) * (j - 1)) / (1.0 * m_dim);
    }
    return matrix;
}

proc within_epsilon(a: real, b: real, eps=1e-6) {
    return abs(a-b) < eps;
}

/* Prints out the 2D structure passed in */
proc print_2D(A: [], m_dim: int, n_dim: int) {
    for i in 1..m_dim {
        write("     ");
        for j in 1..n_dim {
            write(A[i,j], " ");
        }
        writeln();
    }
}

/* The process which runs the benchmark */
proc kernel_syrk(dist_2D, m_dim: int, n_dim: int) {
  var still_correct = true;
    var t:stopwatch;
  
  if messages {
    resetCommDiagnostics();
    startCommDiagnostics();
  }
  
    /******* Start the timer: this is where we do work *******/
  if timeit {
    t = new stopwatch();
    t.start();
  }
  
    var A = initialize_2D(dist_2D, m_dim);
    var C = initialize_2D(dist_2D, m_dim);
    
    C *= beta;
    
    forall i in 1..m_dim with (ref C) {
        forall j in 1..n_dim with (ref C) {
            var temp: real = C[i,j];
            var tempArray: [1..n_dim] real;
            forall (a,b,c) in zip (A[i,..], A[j,..], 1..) with (ref tempArray) {
                tempArray[c] = alpha * a * b; 
            }
            C[i,j] = temp + (+ reduce (tempArray));
        }
    }
  
    /******* End the timer *******/
  if timeit {
      t.stop();
    writeln("took ", t.elapsed(), " seconds");
  }
  
  //Print out communication counts (gets and puts)
  if messages {
    stopCommDiagnostics();  
    var messages=0;
    var coms=getCommDiagnostics();
    for i in 0..numLocales-1 {
      messages+=coms(i).get:int;
      messages+=coms(i).put:int;
    }
    writeln('message count=', messages);  
  }
 
   if correct {
      var ATest = initialize_2D({1..m_dim, 1..n_dim}, m_dim);
      var CTest = initialize_2D({1..m_dim, 1..n_dim}, m_dim);
    
      CTest *= beta;
    
      forall i in 1..m_dim with (ref CTest) {
          forall j in 1..n_dim with (ref CTest) {
              var tempTest: real = CTest[i,j];
              var tempArrayTest: [1..n_dim] real;
              forall (a,b,c) in zip (ATest[i,..], ATest[j,..], 1..) with (ref tempArrayTest) {
                  tempArrayTest[c] = alpha * a * b; 
              }
              CTest[i,j] = tempTest + (+ reduce (tempArrayTest));
          }
      }
    
    for ii in 1..m_dim {
      for jj in 1..m_dim {
        still_correct &&= within_epsilon(C[ii,jj], CTest[ii,jj]);
      }
    }
    writeln("Is the calculation correct? ", still_correct);
    writeln("syrk computation complete.");
   }
    
    if (printData) {
        writeln("A:");
        print_2D(A, m_dim, n_dim);
        writeln("C:");
        print_2D(C, m_dim, n_dim);
    }
}

proc main() {
    /* Initialize the domains */
    var dom_2D = {1..M, 1..N};
    
    var t: stopwatch;
    
    if dist == "NONE" {
        var dist_2D = dom_2D;
        /* Run the benchmark */
        kernel_syrk(dist_2D, M, N); 
    } /*else if dist == "CM" {
        var dist_2D = dom_2D dmapped CyclicZipOpt(startIdx=dom_2D.low);
        kernel_syrk(dist_2D, M, N); 
    } */else if dist == "C" {
        var dist_2D = dom_2D dmapped cyclicDist(startIdx=dom_2D.low);
        kernel_syrk(dist_2D, M, N); 
    } else if dist == "B" {
        var dist_2D = dom_2D dmapped blockDist(boundingBox=dom_2D);
        kernel_syrk(dist_2D, M, N);  
    } 
}
