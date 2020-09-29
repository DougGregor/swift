// RUN: %target-swift-frontend -emit-silgen %s -swift-version 5 -enable-experimental-concurrency | %FileCheck -check-prefix CHECK %s
// REQUIRES: concurrency

import _Concurrency

public protocol DefaultInit {
  init()
}

public actor class A1<T: DefaultInit> {
  var x: Int = 17
  var y: T = T()

  public func f() { }
}

extension Int: DefaultInit { }

func buildIt() {
  _ = A1<Int>()
}

// variable initialization expression of A1.$__actor_storage
// CHECK-LABEL: sil hidden [transparent] [ossa] @$s29synthesized_conformance_actor2A1C03$__C8_storage33_550E67F1F00BFF89F882603E5B70A41BLL12_Concurrency17_NativeActorQueueVvpfi : $@convention(thin) <T where T : DefaultInit> () -> @out _NativeActorQueue {
// CHECK: bb0([[PROPERTY:%.*]] : $*_NativeActorQueue):
// CHECK-NEXT: [[META:%.*]] = metatype $@thick A1<T>.Type
// CHECK-NEXT: [[ERASED_META:%.*]] = init_existential_metatype [[META]] : $@thick A1<T>.Type, $@thick AnyObject.Type
// CHECK: [[INIT_FN:%.*]] = function_ref @$s12_Concurrency24_defaultActorQueueCreateyAA07_NativecD0VyXlXpF : $@convention(thin) (@thick AnyObject.Type) -> @out _NativeActorQueue
// CHECK-NEXT: = apply [[INIT_FN]]([[PROPERTY]], [[ERASED_META]]) : $@convention(thin) (@thick AnyObject.Type) -> @out _NativeActorQueue

// A1.enqueue(partialTask:)
// CHECK-LABEL: sil hidden [ossa] @$s29synthesized_conformance_actor2A1C7enqueue11partialTasky12_Concurrency012PartialAsyncG0V_tF : $@convention(method) <T where T : DefaultInit> (@in_guaranteed PartialAsyncTask, @guaranteed A1<T>) -> () {
// CHECK: bb0([[PARTIAL_TASK:%.*]] : $*PartialAsyncTask, [[SELF:%.*]] : @guaranteed $A1<T>):
// CHECK:       [[SELF_COPY:%.*]] = copy_value [[SELF]] : $A1<T>
// CHECK-NEXT:  [[SELF_ANY_OBJECT:%.*]] = init_existential_ref [[SELF_COPY]] : $A1<T> : $A1<T>, $AnyObject
// CHECK-NEXT:  [[PROPERTY_REF:%.*]] = ref_element_addr [[SELF]] : $A1<T>, #A1.$__actor_storage
// FIXME: Need to eliminate this exclusivity check.
// CHECK-NEXT:  [[DYNAMIC_ACCESS:%.*]] = begin_access [modify] [dynamic] [[PROPERTY_REF]] : $*_NativeActorQueue
// CHECK:       [[ENQUEUE_FN:%.*]] = function_ref @$s12_Concurrency36_defaultActorQueueEnqueuePartialTask5actor5queue07partialG0yyXl_AA07_NativecD0VzAA0f5AsyncG0VtF : $@convention(thin) (@guaranteed AnyObject, @inout _NativeActorQueue, @in_guaranteed PartialAsyncTask) -> ()
// CHECK-NEXT:  apply [[ENQUEUE_FN]]([[SELF_ANY_OBJECT]], [[DYNAMIC_ACCESS]], [[PARTIAL_TASK]]) : $@convention(thin) (@guaranteed AnyObject, @inout _NativeActorQueue, @in_guaranteed PartialAsyncTask) -> ()
// CHECK-NEXT:  end_access [[DYNAMIC_ACCESS]] : $*_NativeActorQueue
