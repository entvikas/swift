// RUN: %target-typecheck-verify-swift

protocol P {}

struct S<T> {}

class C : P {}

extension S where T : P {
  typealias A = Int
  typealias B<U> = S<U>
}

extension S where T == Float {
  typealias C = Int
}

class A<T, U> {}

extension A where T == [U], U: P {
  typealias S1 = Int
}

extension A where T == [U], U == Int {
  typealias S2 = Int
}

class B<U> : A<[U], U> {}

_ = B<C>.S1()          // Ok
_ = B<Int>.S2()        // Ok
_ = B<Float>.S1()      // expected-error {{'B<Float>.S1' (aka 'Int') requires that 'Float' conform to 'P'}}
_ = B<String>.S2()     // expected-error {{'B<String>.S2.Type' (aka 'Int.Type') requires the types '[String]' and '[Int]' be equivalent}}

_ = S<C>.A()           // Ok
_ = S<Int>.A()         // expected-error {{'S<Int>.A' (aka 'Int') requires that 'Int' conform to 'P'}}
_ = S<String>.B<Int>() // expected-error {{type 'String' does not conform to protocol 'P'}}
_ = S<Int>.C()         // expected-error {{'S<Int>.C.Type' (aka 'Int.Type') requires the types 'Int' and 'Float' be equivalent}}

func foo<T>(_ s: S<T>.Type) {
  _ = s.A() // expected-error {{'S<T>.A' (aka 'Int') requires that 'T' conform to 'P'}}
}

func bar<T: P>(_ s: S<T>.Type) {
  _ = s.A() // Ok
}
