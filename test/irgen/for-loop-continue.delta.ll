
%StringIterator = type { i8*, i8* }
%StringRef = type { %"ArrayRef<char>" }
%"ArrayRef<char>" = type { i8*, i32 }

@0 = private unnamed_addr constant [4 x i8] c"abc\00"

define i32 @main() {
  %__iterator = alloca %StringIterator
  %__str0 = alloca %StringRef
  %ch = alloca i8
  call void @_ENM3std9StringRef4initE7pointerP4char6length4uint(%StringRef* %__str0, i8* getelementptr inbounds ([4 x i8], [4 x i8]* @0, i32 0, i32 0), i32 3)
  %__str01 = load %StringRef, %StringRef* %__str0
  %1 = call %StringIterator @_EN3std9StringRef8iteratorE(%StringRef %__str01)
  store %StringIterator %1, %StringIterator* %__iterator
  br label %loop.condition

loop.condition:                                   ; preds = %loop.increment, %0
  %__iterator2 = load %StringIterator, %StringIterator* %__iterator
  %2 = call i1 @_EN3std14StringIterator8hasValueE(%StringIterator %__iterator2)
  br i1 %2, label %loop.body, label %loop.end

loop.body:                                        ; preds = %loop.condition
  %__iterator3 = load %StringIterator, %StringIterator* %__iterator
  %3 = call i8 @_EN3std14StringIterator5valueE(%StringIterator %__iterator3)
  store i8 %3, i8* %ch
  %ch4 = load i8, i8* %ch
  %4 = icmp eq i8 %ch4, 98
  br i1 %4, label %if.then, label %if.else

loop.increment:                                   ; preds = %if.end, %if.then
  call void @_ENM3std14StringIterator9incrementE(%StringIterator* %__iterator)
  br label %loop.condition

loop.end:                                         ; preds = %loop.condition
  ret i32 0

if.then:                                          ; preds = %loop.body
  br label %loop.increment

if.else:                                          ; preds = %loop.body
  br label %if.end

if.end:                                           ; preds = %if.else
  br label %loop.increment
}

declare %StringIterator @_EN3std9StringRef8iteratorE(%StringRef)

declare void @_ENM3std9StringRef4initE7pointerP4char6length4uint(%StringRef*, i8*, i32)

declare i1 @_EN3std14StringIterator8hasValueE(%StringIterator)

declare i8 @_EN3std14StringIterator5valueE(%StringIterator)

declare void @_ENM3std14StringIterator9incrementE(%StringIterator*)
