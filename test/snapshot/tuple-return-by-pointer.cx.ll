
%S = type { { i32 } }

define i32 @main() {
  %s = alloca %S, align 8
  %f = alloca { i32 }*, align 8
  %g = alloca { i32 }*, align 8
  call void @_EN4main1S4initET3int_(%S* %s, { i32 } { i32 1 })
  %1 = call { i32 }* @_EN4main1S1fE(%S* %s)
  store { i32 }* %1, { i32 }** %f, align 8
  %2 = call { i32 }* @_EN4main1S1gE(%S* %s)
  store { i32 }* %2, { i32 }** %g, align 8
  ret i32 0
}

define void @_EN4main1S4initET3int_(%S* %this, { i32 } %t) {
  %t1 = getelementptr inbounds %S, %S* %this, i32 0, i32 0
  store { i32 } %t, { i32 }* %t1, align 4
  ret void
}

define { i32 }* @_EN4main1S1fE(%S* %this) {
  %t = getelementptr inbounds %S, %S* %this, i32 0, i32 0
  ret { i32 }* %t
}

define { i32 }* @_EN4main1S1gE(%S* %this) {
  %t = getelementptr inbounds %S, %S* %this, i32 0, i32 0
  ret { i32 }* %t
}
