
%Foo = type { i32 }

define i32 @main() {
  %foo = alloca %Foo, align 8
  %i = alloca i32, align 4
  call void @_EN4main3Foo4initE(%Foo* %foo)
  call void @_EN4main3Foo3barE(%Foo* %foo)
  %1 = call i32 @_EN4main3Foo3quxE(%Foo* %foo)
  store i32 %1, i32* %i, align 4
  ret i32 0
}

define void @_EN4main3Foo4initE(%Foo* %this) {
  %baz = getelementptr inbounds %Foo, %Foo* %this, i32 0, i32 0
  store i32 42, i32* %baz, align 4
  ret void
}

define void @_EN4main3Foo3barE(%Foo* %this) {
  %baz = getelementptr inbounds %Foo, %Foo* %this, i32 0, i32 0
  %baz.load = load i32, i32* %baz, align 4
  %1 = add i32 %baz.load, 1
  store i32 %1, i32* %baz, align 4
  ret void
}

define i32 @_EN4main3Foo3quxE(%Foo* %this) {
  %baz = getelementptr inbounds %Foo, %Foo* %this, i32 0, i32 0
  %baz.load = load i32, i32* %baz, align 4
  ret i32 %baz.load
}
