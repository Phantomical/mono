; The shapes a promoted body arrives at the tier-2 inliner in, with the counts
; the profile leaves on them. inline-heat-tests.cpp reads that ranking for a
; call site in each.
;
; Every body is entered 1000 times, so a count below is a share of that and the
; verdict can be read off the ratio. "mono-tier-threshold" is what marks a body
; the tier-2 counter selected.
;
; The module summary is modelled on one a real tier-2 compile carries: it holds
; @looping's two count levels and nothing else, because such a compile holds one
; promoted body. Its cold percentile therefore lands on that body's own entry
; count, which is what makes @looping's entry block read cold to LLVM.

declare i32 @opaque(i32)

; A body to weigh. Two arms, so it earns no SingleBBBonus and the budget a test
; reads is the threshold alone.
define i32 @callee(i32 %n) {
entry:
  %small = icmp slt i32 %n, 8
  br i1 %small, label %low, label %high

low:
  %a = add i32 %n, 1
  ret i32 %a

high:
  %b = add i32 %n, 2
  ret i32 %b
}

; An entry block the body always runs, and a loop that takes 4096 turns for each
; entry. This is the shape the tier-2 counter promotes: the loop is what spent
; the counter and the entry block is what the body always does.
define i32 @looping(i32 %n) #0 !prof !0 {
entry:
  %a = call i32 @callee(i32 %n)
  br label %body

body:
  %b = call i32 @callee(i32 %n)
  %again = icmp slt i32 %b, %n
  br i1 %again, label %body, label %done, !prof !1

done:
  ret i32 %b
}

; No loop, so every block runs each time the body is entered.
define i32 @loopless(i32 %n) #0 !prof !0 {
entry:
  %a = call i32 @callee(i32 %n)
  ret i32 %a
}

; A block taken 19 times for every 1000 entries, which is under the cold share.
define i32 @rare_under(i32 %n) #0 !prof !0 {
entry:
  %take = icmp slt i32 %n, 0
  br i1 %take, label %rare, label %done, !prof !2

rare:
  %a = call i32 @opaque(i32 %n)
  br label %done

done:
  ret i32 %n
}

; The same block taken 20 times for every 1000 entries, which is not.
define i32 @rare_at(i32 %n) #0 !prof !0 {
entry:
  %take = icmp slt i32 %n, 0
  br i1 %take, label %rare, label %done, !prof !3

rare:
  %a = call i32 @opaque(i32 %n)
  br label %done

done:
  ret i32 %n
}

attributes #0 = { "mono-tier-threshold"="100000000" }

!llvm.module.flags = !{!4}

!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 4095, i32 1}
!2 = !{!"branch_weights", i32 19, i32 981}
!3 = !{!"branch_weights", i32 20, i32 980}

!4 = !{i32 1, !"ProfileSummary", !5}
!5 = !{!6, !7, !8, !9, !10, !11, !12, !13, !14, !15}
!6 = !{!"ProfileFormat", !"InstrProf"}
!7 = !{!"TotalCount", i64 4097000}
!8 = !{!"MaxCount", i64 4096000}
!9 = !{!"MaxInternalCount", i64 4096000}
!10 = !{!"MaxFunctionCount", i64 4096000}
!11 = !{!"NumCounts", i64 2}
!12 = !{!"NumFunctions", i64 1}
!13 = !{!"IsPartialProfile", i64 0}
!14 = !{!"PartialProfileRatio", double 0.000000e+00}
!15 = !{!"DetailedSummary", !16}
!16 = !{!17, !18, !19}
!17 = !{i32 10000, i64 4096000, i32 1}
!18 = !{i32 990000, i64 4096000, i32 1}
!19 = !{i32 999999, i64 1000, i32 2}
