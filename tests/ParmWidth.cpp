#include "UeApi/Types.h"

#include "UeApi/Engine.h"

UE_MOD_PACKAGE("/Game/_ElytrasMods/ParmWidth");

/*
The widest parameter list UFunction can count: NumParms is a uint8 and the return value is a parameter too,
so 254 arguments and a result are the most one function may have. This one sits exactly on that edge and
must still cook and run; one more argument must be refused rather than wrap the count.
*/
class ParmWidth : public AActor {
public:
  int32 Edge(int32 A0, int32 A1, int32 A2, int32 A3, int32 A4, int32 A5, int32 A6, int32 A7, int32 A8, int32 A9,
             int32 A10, int32 A11, int32 A12, int32 A13, int32 A14, int32 A15, int32 A16, int32 A17, int32 A18,
             int32 A19, int32 A20, int32 A21, int32 A22, int32 A23, int32 A24, int32 A25, int32 A26, int32 A27,
             int32 A28, int32 A29, int32 A30, int32 A31, int32 A32, int32 A33, int32 A34, int32 A35, int32 A36,
             int32 A37, int32 A38, int32 A39, int32 A40, int32 A41, int32 A42, int32 A43, int32 A44, int32 A45,
             int32 A46, int32 A47, int32 A48, int32 A49, int32 A50, int32 A51, int32 A52, int32 A53, int32 A54,
             int32 A55, int32 A56, int32 A57, int32 A58, int32 A59, int32 A60, int32 A61, int32 A62, int32 A63,
             int32 A64, int32 A65, int32 A66, int32 A67, int32 A68, int32 A69, int32 A70, int32 A71, int32 A72,
             int32 A73, int32 A74, int32 A75, int32 A76, int32 A77, int32 A78, int32 A79, int32 A80, int32 A81,
             int32 A82, int32 A83, int32 A84, int32 A85, int32 A86, int32 A87, int32 A88, int32 A89, int32 A90,
             int32 A91, int32 A92, int32 A93, int32 A94, int32 A95, int32 A96, int32 A97, int32 A98, int32 A99,
             int32 A100, int32 A101, int32 A102, int32 A103, int32 A104, int32 A105, int32 A106, int32 A107,
             int32 A108, int32 A109, int32 A110, int32 A111, int32 A112, int32 A113, int32 A114, int32 A115,
             int32 A116, int32 A117, int32 A118, int32 A119, int32 A120, int32 A121, int32 A122, int32 A123,
             int32 A124, int32 A125, int32 A126, int32 A127, int32 A128, int32 A129, int32 A130, int32 A131,
             int32 A132, int32 A133, int32 A134, int32 A135, int32 A136, int32 A137, int32 A138, int32 A139,
             int32 A140, int32 A141, int32 A142, int32 A143, int32 A144, int32 A145, int32 A146, int32 A147,
             int32 A148, int32 A149, int32 A150, int32 A151, int32 A152, int32 A153, int32 A154, int32 A155,
             int32 A156, int32 A157, int32 A158, int32 A159, int32 A160, int32 A161, int32 A162, int32 A163,
             int32 A164, int32 A165, int32 A166, int32 A167, int32 A168, int32 A169, int32 A170, int32 A171,
             int32 A172, int32 A173, int32 A174, int32 A175, int32 A176, int32 A177, int32 A178, int32 A179,
             int32 A180, int32 A181, int32 A182, int32 A183, int32 A184, int32 A185, int32 A186, int32 A187,
             int32 A188, int32 A189, int32 A190, int32 A191, int32 A192, int32 A193, int32 A194, int32 A195,
             int32 A196, int32 A197, int32 A198, int32 A199, int32 A200, int32 A201, int32 A202, int32 A203,
             int32 A204, int32 A205, int32 A206, int32 A207, int32 A208, int32 A209, int32 A210, int32 A211,
             int32 A212, int32 A213, int32 A214, int32 A215, int32 A216, int32 A217, int32 A218, int32 A219,
             int32 A220, int32 A221, int32 A222, int32 A223, int32 A224, int32 A225, int32 A226, int32 A227,
             int32 A228, int32 A229, int32 A230, int32 A231, int32 A232, int32 A233, int32 A234, int32 A235,
             int32 A236, int32 A237, int32 A238, int32 A239, int32 A240, int32 A241, int32 A242, int32 A243,
             int32 A244, int32 A245, int32 A246, int32 A247, int32 A248, int32 A249, int32 A250, int32 A251,
             int32 A252, int32 A253) {
    return A0 - A126 + A253;
  }
};
