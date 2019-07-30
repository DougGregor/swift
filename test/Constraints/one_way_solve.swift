// RUN: %target-typecheck-verify-swift -enable-solver-one-way-constraints -parse-stdlib -debug-constraints > %t.log 2>&1
// RUN: %FileCheck %s < %t.log
import Swift


func takeDoubleAndBool(_: Double, _: Bool) { }

func testTernaryOneWay(b: Bool, b2: Bool) {
  // CHECK: ---Connected components---
  // CHECK-NEXT: 0: $T4 $T5 $T7 $T8 $T9 $T10, one way components = {$T7} {$T4} {$T5 $T8 $T9 depends on 1, 0} {$T10 depends on 1, 0, 2}
  // CHECK-NEXT: $T11 $T13 $T14
  takeDoubleAndBool(
    Builtin.one_way(
      b ? Builtin.one_way(3.14159) : Builtin.one_way(2.71828)),
    b == true)
}
