; The shapes inline-cost-dom-chain-tests.cpp weighs. Each callee mirrors a mono
; type-test cascade's own guard: a comparison against null whose Value the cost
; model cannot fold to a constant on its own, several blocks below whatever
; already proved that Value non-null. What differs between callees is whether
; anything upstream proves it, which is what decides whether findDeadBlocks ()
; can keep @expensive's three calls out of the price.

declare void @expensive()
declare i32 @opaque(i32)
declare ptr @opaque_ptr()

; A null check, two unrelated tests standing in for the rest of a cascade, and
; then the guard that matters -- three one-predecessor blocks between the proof
; and the guard it has to reach, the shape a LINQ builder's own source cascade
; has past its first couple of tests.
define ptr @dominated(ptr %o) {
entry:
  %isnull = icmp eq ptr %o, null
  br i1 %isnull, label %throw, label %check1

throw:
  unreachable

check1:
  %a = call i32 @opaque(i32 0)
  %a.eq = icmp eq i32 %a, 0
  br i1 %a.eq, label %check2, label %other1

check2:
  %b = call i32 @opaque(i32 1)
  %b.eq = icmp eq i32 %b, 0
  br i1 %b.eq, label %guard, label %other2

guard:
  %g = icmp eq ptr %o, null
  br i1 %g, label %dead, label %live

dead:
  call void @expensive()
  call void @expensive()
  call void @expensive()
  br label %join

live:
  br label %join

join:
  ret ptr %o

other1:
  ret ptr null

other2:
  ret ptr null
}

; The same shape with the leading null check gone, so nothing above %guard
; proves %o non-null and @expensive stays priced.
define ptr @undominated(ptr %o) {
entry:
  br label %check1

check1:
  %a = call i32 @opaque(i32 0)
  %a.eq = icmp eq i32 %a, 0
  br i1 %a.eq, label %check2, label %other1

check2:
  %b = call i32 @opaque(i32 1)
  %b.eq = icmp eq i32 %b, 0
  br i1 %b.eq, label %guard, label %other2

guard:
  %g = icmp eq ptr %o, null
  br i1 %g, label %dead, label %live

dead:
  call void @expensive()
  call void @expensive()
  call void @expensive()
  br label %join

live:
  br label %join

join:
  ret ptr %o

other1:
  ret ptr null

other2:
  ret ptr null
}

; %o's own null check answers straight off a formal argument, the one shape
; isKnownNonNullInCallee () already read before this file's fix: a NonNull
; attribute on the call site. root_raw_operand's own call passes an opaque,
; unattributed pointer for this argument, and attaches nonnull to a
; different one, so only the direct argument answers and a caller-side
; substitution of it must not be the only path tried.
define ptr @raw_operand(ptr %unused, ptr %o) {
entry:
  %isnull = icmp eq ptr %o, null
  br i1 %isnull, label %dead, label %live

dead:
  call void @expensive()
  call void @expensive()
  call void @expensive()
  br label %join

live:
  br label %join

join:
  ret ptr %o
}

define ptr @root_dominated(ptr %p) {
entry:
  %r = call ptr @dominated(ptr %p)
  ret ptr %r
}

define ptr @root_undominated(ptr %p) {
entry:
  %r = call ptr @undominated(ptr %p)
  ret ptr %r
}

; %arbitrary carries no attribute or alloca of its own, so a Settled resolved
; to it can never answer. %p is nonnull only at this call site, put there the
; way an already-proven caller-side fact would be, not on %arbitrary's own
; argument position.
define ptr @root_raw_operand(ptr %p) {
entry:
  %arbitrary = call ptr @opaque_ptr()
  %r = call ptr @raw_operand(ptr %arbitrary, ptr nonnull %p)
  ret ptr %r
}

; The same call with the nonnull attribute gone, so nothing answers either
; operand and @raw_operand's dead arm is priced -- what root_raw_operand's
; own cost is measured against.
define ptr @root_raw_operand_unproven(ptr %p) {
entry:
  %arbitrary = call ptr @opaque_ptr()
  %r = call ptr @raw_operand(ptr %arbitrary, ptr %p)
  ret ptr %r
}
