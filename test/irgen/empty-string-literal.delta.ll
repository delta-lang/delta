
%string = type { %"ArrayRef<char>" }
%"ArrayRef<char>" = type { i8*, i32 }

@0 = private unnamed_addr constant [1 x i8] zeroinitializer, align 1

define i32 @main() {
  %__str4 = alloca %string
  call void @_EN3std6string4initE7pointerP4char6length3int(%string* %__str4, i8* getelementptr inbounds ([1 x i8], [1 x i8]* @0, i32 0, i32 0), i32 0)
  %1 = call i32 @_EN3std6string4sizeE(%string* %__str4)
  ret i32 %1
}

declare i32 @_EN3std6string4sizeE(%string*)

declare void @_EN3std6string4initE7pointerP4char6length3int(%string*, i8*, i32)
