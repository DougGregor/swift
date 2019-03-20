// RUN: %target-typecheck-verify-swift -debugger-support

@propertyDelegate
struct Wrapper<T> { // expected-note{{generic struct 'Wrapper' declared here}}
  var value: T
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

func testParsing() {
  var wrapped1: Int by Wrapper
  var wrapped2: Int by Wrapper

  _ = wrapped1
  _ = wrapped2
}

func testParseError() {
  let (a, b): (Int, Int) by Wrapper // expected-error{{property delegate can only by written on a single-variable pattern}}
  let (c, d): (Int, Int) by // expected-error{{expected property delegate type after 'by'}}

  _ = a
  _ = b
  _ = c
  _ = d
}

// ---------------------------------------------------------------------------
// Type formation
// ---------------------------------------------------------------------------
func testExplicitWrapperType() {
  var wrapped1: Int by Wrapper

  wrapped1 = "Hello" // expected-error{{cannot assign value of type 'String' to type 'Int'}}
}

@propertyDelegate
struct NonGenericWrapper { }
// expected-error@-1{{property delegate type must have a single generic type parameter}}

@propertyDelegate
struct TwoParameterWrapper<T, U> { }
// expected-error@-1{{property delegate type must have a single generic type parameter}}

@propertyDelegate
struct MissingValue<T> { }
// expected-error@-1{{property delegate type 'MissingValue' does not contain a non-static property named 'value'}}

// expected-note@+1{{type 'NotADelegate<T>' must have the attribute '@propertyDelegate' to be used as a property delegate}}{{1-1=@propertyDelegate}}
struct NotADelegate<T> {
  var value: T
}

// expected-error@+1{{'@propertyDelegate' attribute cannot be applied to this declaration}}
@propertyDelegate
protocol CannotBeADelegate {
  associatedtype Value
  var value: Value { get set }
}

func testBadWrapperTypes() {
  var wrapped1: Int by NonGenericWrapper // expected-error{{property delegate type 'NonGenericWrapper' must not provide generic arguments}}
  var wrapped2: Int by TwoParameterWrapper
  var wrapped3: Int by TwoParameterWrapper<Int, Int> // expected-error{{property delegate type 'TwoParameterWrapper<Int, Int>' must not provide generic arguments}}
  var wrapped4: Int by (Int) // expected-error{{use of non-property delegate type 'Int' as a property delegate}}
  var wrapped5: Int by Wrapper<Int> // expected-error{{property delegate type 'Wrapper<Int>' must not provide generic arguments}}
  var wrapped6: Int by NotADelegate // expected-error{{use of non-property delegate type 'NotADelegate' as a property delegate}}

  wrapped1 = 0
  wrapped2 = 0
  wrapped3 = 0
  wrapped4 = 0
  wrapped5 = 0
  wrapped6 = 0
  _ = wrapped1
  _ = wrapped2
  _ = wrapped3
  _ = wrapped4
  _ = wrapped5
  _ = wrapped6
}

// ---------------------------------------------------------------------------
// Property delegates as members
// ---------------------------------------------------------------------------
struct HasDelegate {
  var wrapped1: Int by Wrapper
}

// ---------------------------------------------------------------------------
// Initial value initializers
// ---------------------------------------------------------------------------
@propertyDelegate
struct WrapperWithInitialValue<T> {
  var value: T

  init(initialValue: T) {
    self.value = initialValue
  }
}

func testInitialValueInference(i: Int, s: String) {
  // Inferring the type of the property itself
  var x by WrapperWithInitialValue = i
  x = 3.14159 // expected-error{{cannot assign value of type 'Double' to type 'Int'}}

  // Inferring part of the type of the property itself
  var y: Dictionary by WrapperWithInitialValue = [s: i]
  y = 3.14159 // expected-error{{cannot assign value of type 'Double' to type 'Dictionary<String, Int>'}}
}

func testInitialValueWithoutDelegateSupport(i: Int) {
  var x by Wrapper = i // expected-error{{cannot directly initialize property 'x' with property delegate 'Wrapper' that lacks an 'init(initialValue:)' initializer}}
}



@propertyDelegate
struct WrapperWithAmbiguousInitialValue<T> { // expected-error{{property delegate type 'WrapperWithAmbiguousInitialValue' has multiple initial-value initializers}}
  var value: T

  init(initialValue: T?) { // expected-note{{initializer 'init(initialValue:)' declared here}}
    self.value = initialValue!
  }

  init(initialValue: T) { // expected-note{{initializer 'init(initialValue:)' declared here}}
    self.value = initialValue
  }
}
