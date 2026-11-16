set(proj_src ${CMAKE_CURRENT_SOURCE_DIR})
set(proj_bin ${CMAKE_CURRENT_BINARY_DIR})

set(rt_flags -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie
             -fno-common -fno-ident -march=armv8.1-a -O2 -Wall)

add_custom_command(OUTPUT ${proj_bin}/rt_core.o
    COMMAND ${CMAKE_C_COMPILER} -c ${rt_flags}
            ${proj_src}/payload.c -o ${proj_bin}/rt_core.o
    DEPENDS ${proj_src}/payload.c ${proj_src}/mnk.h)

add_custom_command(OUTPUT ${proj_bin}/rt_asm.o
    COMMAND ${CMAKE_C_COMPILER} -c ${rt_flags}
            ${proj_src}/asm.S -o ${proj_bin}/rt_asm.o
    DEPENDS ${proj_src}/asm.S)

function(add_blob elf_target blob_symbol_prefix out_blob)
    string(REPLACE "." "_" sym_src ${elf_target})
    add_custom_command(OUTPUT ${proj_bin}/${out_blob}
        COMMAND ${CMAKE_OBJCOPY} -I binary -O elf64-littleaarch64
                --redefine-sym _binary_${sym_src}_start=${blob_symbol_prefix}_start
                --redefine-sym _binary_${sym_src}_end=${blob_symbol_prefix}_end
                ${elf_target} ${out_blob}
        WORKING_DIRECTORY ${proj_bin}
        DEPENDS ${proj_bin}/${elf_target})
endfunction()

function(add_runtime_image name base pool mark trace cstack gstack)
    add_custom_command(OUTPUT ${proj_bin}/rt_${name}.elf
        COMMAND ${CMAKE_C_COMPILER} -nostdlib -static -no-pie
                -T ${proj_src}/payload.ld -Wl,--build-id=none
                -Wl,--defsym=IMAGE_BASE=0x${base}
                -Wl,--defsym=POOL_VA=0x${pool}
                -Wl,--defsym=MARK_VA=0x${mark}
                -Wl,--defsym=TRACE_VA=0x${trace}
                -Wl,--defsym=CSTACK_VA=0x${cstack}
                -Wl,--defsym=GSTACK_VA=0x${gstack}
                ${proj_bin}/rt_core.o ${proj_bin}/rt_asm.o
                -o ${proj_bin}/rt_${name}.elf
        DEPENDS ${proj_bin}/rt_core.o ${proj_bin}/rt_asm.o
                ${proj_src}/payload.ld)
    add_blob(rt_${name}.elf rt_${name} rt_${name}_blob.o)
endfunction()

add_runtime_image(a 40000000 42000000 42100000 42200000 42400000 42800000)
add_runtime_image(b 60000000 62000000 62100000 62200000 62400000 62800000)

function(add_user_program name)
    add_custom_command(OUTPUT ${proj_bin}/prog_${name}.o
        COMMAND ${CMAKE_C_COMPILER} -c -fPIC -ffreestanding -fno-builtin
                -fno-stack-protector -fno-pie -fno-common -fno-ident -O2 -Wall
                -I ${proj_src}/../include
                ${proj_src}/../user/program${name}.c -o ${proj_bin}/prog_${name}.o
        DEPENDS ${proj_src}/../user/program${name}.c ${proj_src}/../include/mnk_user.h)
    add_custom_command(OUTPUT ${proj_bin}/prog${name}.so
        COMMAND ${CMAKE_C_COMPILER} -nostdlib -shared -e program_main
                -Wl,--build-id=none ${proj_bin}/prog_${name}.o
                -o ${proj_bin}/prog${name}.so
        DEPENDS ${proj_bin}/prog_${name}.o)
    add_blob(prog${name}.so prog${name} prog${name}_blob.o)
endfunction()

add_user_program(1)
add_user_program(2)
add_user_program(3)
add_user_program(4)
add_user_program(5)

add_executable(mnk-sel4-demo main.c
    ${proj_bin}/rt_a_blob.o ${proj_bin}/rt_b_blob.o
    ${proj_bin}/prog1_blob.o ${proj_bin}/prog2_blob.o ${proj_bin}/prog3_blob.o
    ${proj_bin}/prog4_blob.o ${proj_bin}/prog5_blob.o)
target_link_libraries(mnk-sel4-demo muslc sel4 sel4runtime sel4muslcsys utils)
DeclareRootserver(mnk-sel4-demo)
