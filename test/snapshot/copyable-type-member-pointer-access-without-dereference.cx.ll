
%S = type { i32* }

define i32 @main() {
  %1 = alloca %S, align 8
  %2 = alloca i32, align 4
  store i32 1, i32* %2, align 4
  call void @_EN4main1S4initEP3int(%S* %1, i32* %2)
  call void @_EN4main1S3fooE(%S* %1)
  ret i32 0
}

define void @_EN4main1S4initEP3int(%S* %this, i32* %a) {
  %a1 = getelementptr inbounds %S, %S* %this, i32 0, i32 0
  store i32* %a, i32** %a1, align 8
  ret void
}

define void @_EN4main1S3fooE(%S* %this) {
  %b = alloca i32*, align 8
  %a = getelementptr inbounds %S, %S* %this, i32 0, i32 0
  %a.load = load i32*, i32** %a, align 8
  store i32* %a.load, i32** %b, align 8
  ret void
}
