
%"UniquePtr<UniquePtr<int>>" = type { %"UniquePtr<int>"* }
%"UniquePtr<int>" = type { i32* }

@0 = private unnamed_addr constant [38 x i8] c"Unwrap failed at allocate.delta:9:54\0A\00", align 1
@1 = private unnamed_addr constant [38 x i8] c"Unwrap failed at allocate.delta:9:54\0A\00", align 1

define i32 @main() {
  %p = alloca %"UniquePtr<UniquePtr<int>>"
  %1 = alloca %"UniquePtr<int>"
  %q = alloca %"UniquePtr<int>"
  %r = alloca %"UniquePtr<int>"
  call void @_EN3std9UniquePtrI3intE4initE3int(%"UniquePtr<int>"* %1, i32 42)
  %.load = load %"UniquePtr<int>", %"UniquePtr<int>"* %1
  call void @_EN3std9UniquePtrI9UniquePtrI3intEE4initE9UniquePtrI3intE(%"UniquePtr<UniquePtr<int>>"* %p, %"UniquePtr<int>" %.load)
  call void @_EN3std9UniquePtrI3intE4initE3int(%"UniquePtr<int>"* %q, i32 42)
  call void @_EN3std9UniquePtrI3intE4initE3int(%"UniquePtr<int>"* %r, i32 42)
  %2 = call i32* @_EN3std9UniquePtrI3intE3getE(%"UniquePtr<int>"* %q)
  store i32 0, i32* %2
  %3 = call i32* @_EN3std9UniquePtrI3intE3getE(%"UniquePtr<int>"* %r)
  call void @_EN3std9UniquePtrI3intE6deinitE(%"UniquePtr<int>"* %q)
  %r.load = load %"UniquePtr<int>", %"UniquePtr<int>"* %r
  store %"UniquePtr<int>" %r.load, %"UniquePtr<int>"* %q
  call void @_EN3std9UniquePtrI3intE6deinitE(%"UniquePtr<int>"* %q)
  call void @_EN3std9UniquePtrI9UniquePtrI3intEE6deinitE(%"UniquePtr<UniquePtr<int>>"* %p)
  ret i32 0
}

define void @_EN3std9UniquePtrI9UniquePtrI3intEE6deinitE(%"UniquePtr<UniquePtr<int>>"* %this) {
  %pointer = getelementptr inbounds %"UniquePtr<UniquePtr<int>>", %"UniquePtr<UniquePtr<int>>"* %this, i32 0, i32 0
  %pointer.load = load %"UniquePtr<int>"*, %"UniquePtr<int>"** %pointer
  call void @_EN3std10deallocateIP9UniquePtrI3intEEEP9UniquePtrI3intE(%"UniquePtr<int>"* %pointer.load)
  ret void
}

define void @_EN3std9UniquePtrI9UniquePtrI3intEE4initE9UniquePtrI3intE(%"UniquePtr<UniquePtr<int>>"* %this, %"UniquePtr<int>" %value) {
  %pointer = getelementptr inbounds %"UniquePtr<UniquePtr<int>>", %"UniquePtr<UniquePtr<int>>"* %this, i32 0, i32 0
  %1 = call %"UniquePtr<int>"* @_EN3std8allocateI9UniquePtrI3intEEE9UniquePtrI3intE(%"UniquePtr<int>" %value)
  store %"UniquePtr<int>"* %1, %"UniquePtr<int>"** %pointer
  ret void
}

define void @_EN3std9UniquePtrI3intE4initE3int(%"UniquePtr<int>"* %this, i32 %value) {
  %pointer = getelementptr inbounds %"UniquePtr<int>", %"UniquePtr<int>"* %this, i32 0, i32 0
  %1 = call i32* @_EN3std8allocateI3intEE3int(i32 %value)
  store i32* %1, i32** %pointer
  ret void
}

define void @_EN3std9UniquePtrI3intE6deinitE(%"UniquePtr<int>"* %this) {
  %pointer = getelementptr inbounds %"UniquePtr<int>", %"UniquePtr<int>"* %this, i32 0, i32 0
  %pointer.load = load i32*, i32** %pointer
  call void @_EN3std10deallocateIP3intEEP3int(i32* %pointer.load)
  ret void
}

define i32* @_EN3std9UniquePtrI3intE3getE(%"UniquePtr<int>"* %this) {
  %pointer = getelementptr inbounds %"UniquePtr<int>", %"UniquePtr<int>"* %this, i32 0, i32 0
  %pointer.load = load i32*, i32** %pointer
  ret i32* %pointer.load
}

define i32* @_EN3std8allocateI3intEE3int(i32 %value) {
  %allocation = alloca i32*
  %1 = call i8* @malloc(i64 ptrtoint (i32* getelementptr (i32, i32* null, i32 1) to i64))
  %assert.condition = icmp eq i8* %1, null
  br i1 %assert.condition, label %assert.fail, label %assert.success

assert.fail:                                      ; preds = %0
  call void @_EN3std10assertFailEP4char(i8* getelementptr inbounds ([38 x i8], [38 x i8]* @0, i32 0, i32 0))
  unreachable

assert.success:                                   ; preds = %0
  %2 = bitcast i8* %1 to i32*
  store i32* %2, i32** %allocation
  %allocation.load = load i32*, i32** %allocation
  store i32 %value, i32* %allocation.load
  %allocation.load1 = load i32*, i32** %allocation
  ret i32* %allocation.load1
}

define void @_EN3std10deallocateIP3intEEP3int(i32* %allocation) {
  %1 = bitcast i32* %allocation to i8*
  call void @free(i8* %1)
  ret void
}

define %"UniquePtr<int>"* @_EN3std8allocateI9UniquePtrI3intEEE9UniquePtrI3intE(%"UniquePtr<int>" %value) {
  %allocation = alloca %"UniquePtr<int>"*
  %1 = call i8* @malloc(i64 ptrtoint (i1** getelementptr (i1*, i1** null, i32 1) to i64))
  %assert.condition = icmp eq i8* %1, null
  br i1 %assert.condition, label %assert.fail, label %assert.success

assert.fail:                                      ; preds = %0
  call void @_EN3std10assertFailEP4char(i8* getelementptr inbounds ([38 x i8], [38 x i8]* @1, i32 0, i32 0))
  unreachable

assert.success:                                   ; preds = %0
  %2 = bitcast i8* %1 to %"UniquePtr<int>"*
  store %"UniquePtr<int>"* %2, %"UniquePtr<int>"** %allocation
  %allocation.load = load %"UniquePtr<int>"*, %"UniquePtr<int>"** %allocation
  store %"UniquePtr<int>" %value, %"UniquePtr<int>"* %allocation.load
  %allocation.load1 = load %"UniquePtr<int>"*, %"UniquePtr<int>"** %allocation
  ret %"UniquePtr<int>"* %allocation.load1
}

define void @_EN3std10deallocateIP9UniquePtrI3intEEEP9UniquePtrI3intE(%"UniquePtr<int>"* %allocation) {
  %1 = bitcast %"UniquePtr<int>"* %allocation to i8*
  call void @free(i8* %1)
  ret void
}

declare void @free(i8*)

declare i8* @malloc(i64)

declare void @_EN3std10assertFailEP4char(i8*)
