// RUN: %target-typecheck-verify-swift -swift-version 4 -enable-solver-one-way-constraints

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
