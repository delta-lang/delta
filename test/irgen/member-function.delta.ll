
%Foo = type { i32 }

define void @_ENM4main3Foo4initE(%Foo* %this) {
  %baz = getelementptr inbounds %Foo, %Foo* %this, i32 0, i32 0
  store i32 42, i32* %baz
  ret void
}

define void @_ENM4main3Foo3barE(%Foo* %this) {
  %baz = getelementptr inbounds %Foo, %Foo* %this, i32 0, i32 0
  %1 = load i32, i32* %baz
  %2 = add i32 %1, 1
  store i32 %2, i32* %baz
  ret void
}

define i32 @_EN4main3Foo3quxE(%Foo* %this) {
  %baz = getelementptr inbounds %Foo, %Foo* %this, i32 0, i32 0
  %1 = load i32, i32* %baz
  ret i32 %1
}

define i32 @main() {
  %foo = alloca %Foo
  %i = alloca i32
  call void @_ENM4main3Foo4initE(%Foo* %foo)
  call void @_ENM4main3Foo3barE(%Foo* %foo)
  %1 = call i32 @_EN4main3Foo3quxE(%Foo* %foo)
  store i32 %1, i32* %i
  ret i32 0
}