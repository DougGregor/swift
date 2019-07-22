// RUN: %target-typecheck-verify-swift -swift-version 4 -enable-solver-one-way-constraints -parse-stdlib
import Swift

enum Color {
  case red
}

func acceptColorProducer(_: () -> Color) { }

// One-way constraint on the result of single-expression closures.
// FIXME: Diagnostic is bogus!
acceptColorProducer { .red } // expected-error{{cannot convert value of type '() -> Color' to expected argument type '() -> Color'}}
acceptColorProducer { Color.red }

// One-way constraint on the parameters of single-expression closures.
func intToInt(_ a: Int) -> Int { return a }
let c1 = { x, y in intToInt(x + y) } // expected-error{{ambiguous reference to member '+'}}

// Still works because we're able to try both Int and Double for the literal 0.
func onlyDoubles(x: Double, y: Double) -> Double { return x + y }

func acceptBinaryFunc<T>(_: T, body: (T, T) -> T) { }

acceptBinaryFunc(0) { x, y in
  onlyDoubles(x: x, y: y)
}

// Explicit one-way constraints for testing purposes.
func testTernaryOneWay(b: Bool) {
  // Okay: backward inference works.
  let _: Float = b ? 3.14159 : 17

  // Errors due to one-way inference.
  let _: Float = b ? Builtin.one_way(3.14159) // expected-error{{cannot convert value of type 'Double' to specified type 'Float'}}
                   : 17
  let _: Float = b ? 3.14159
                   : Builtin.one_way(17) // expected-error{{cannot convert value of type 'Int' to specified type 'Float'}}
  let _: Float = b ? Builtin.one_way(3.14159) // expected-error{{cannot convert value of type 'Double' to specified type 'Float'}}
                   : Builtin.one_way(17)

  // Okay: default still works.
  let _: Double = b ? Builtin.one_way(3.14159) : 17
  let _: Double = b ? 3.14159 : Builtin.one_way(17)
}
