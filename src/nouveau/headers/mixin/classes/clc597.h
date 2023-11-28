/*
 * Copyright © 2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#define NVC597_SET_MESH_ENABLE                                                                             0x114c
#define NVC597_SET_MESH_ENABLE_V                                                                              0:0
#define NVC597_SET_MESH_ENABLE_V_FALSE                                                                 0x00000000
#define NVC597_SET_MESH_ENABLE_V_TRUE                                                                  0x00000001

#define NVC597_SET_MESH_LAYOUT                                                                             0x1150
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY                                                                       4:0
#define NVC597_SET_MESH_LAYOUT_MAX_VERTICES                                                                  13:4
#define NVC597_SET_MESH_LAYOUT_MAX_PRIMITIVES                                                               22:13
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_POINTS                                                         0x00000000
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_LINES                                                          0x00000001
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_LINE_LOOP                                                      0x00000002
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_LINE_STRIP                                                     0x00000003
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_TRIANGLES                                                      0x00000004
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_TRIANGLE_STRIP                                                 0x00000005
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_TRIANGLE_FAN                                                   0x00000006
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_QUADS                                                          0x00000007
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_QUAD_STRIP                                                     0x00000008
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_POLYGON                                                        0x00000009
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_LINELIST_ADJCY                                                 0x0000000A
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_LINESTRIP_ADJCY                                                0x0000000B
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_TRIANGLELIST_ADJCY                                             0x0000000C
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_TRIANGLESTRIP_ADJCY                                            0x0000000D
#define NVC597_SET_MESH_LAYOUT_TOPOLOGY_PATCH                                                          0x0000000E

#define NVC597_SET_MESH_LOCAL_SIZE                                                                         0x1154
#define NVC597_SET_MESH_LOCAL_SIZE_INVOCATION_COUNT                                                         31:20

#define NVC597_SET_TASK_LAYOUT                                                                             0x1158
#define NVC597_SET_TASK_LAYOUT_INVOCATION_COUNT                                                              11:0
#define NVC597_SET_TASK_LAYOUT_UNK12                                                                        31:12
