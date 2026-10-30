/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_apfs_module_v1.h"
#include "ifs_apfs_fixture_host_v1.h"
#include <errno.h>
int ifs_apfs_worker_main(int,char **);
int ldtm_format_apfs_volume(const char *);
int ldtm_verify_apfs_payload(const char *,char *,size_t);
int ldtm_verify_apfs_payload_after_defrag(const char *,char *,size_t);
int ifs_bind_services(const IfsDefragServicesV1 *);
void ifs_unbind_services(void);
int ifs_bind_fixture(const IfsApfsFixtureHostV1 *);
void ifs_unbind_fixture(void);
static int run(int argc,char **argv,const IfsDefragServicesV1 *host) {
 if(ifs_bind_services(host)!=0) return 1;
 int result=ifs_apfs_worker_main(argc,argv);
 ifs_unbind_services(); return result;
}
static int fixture(uint32_t op,const char *path,const void *unused,
 char *detail,size_t cap,const IfsDefragServicesV1 *services,const void *corpus) {
 (void)unused;
 if(!path || ifs_bind_services(services)!=0) return -1;
 if(ifs_bind_fixture((const IfsApfsFixtureHostV1 *)corpus)!=0) {ifs_unbind_services();return -1;}
 int r=-1;
 switch(op) {
 case IFS_APFS_FIXTURE_FORMAT: r=ldtm_format_apfs_volume(path); break;
 case IFS_APFS_FIXTURE_VERIFY: r=ldtm_verify_apfs_payload(path,detail,cap); break;
 case IFS_APFS_FIXTURE_VERIFY_DEFRAG: r=ldtm_verify_apfs_payload_after_defrag(path,detail,cap); break;
 default: errno=EINVAL; break;
 }
 ifs_unbind_fixture();ifs_unbind_services();return r;
}
static const IfsApfsModuleV1 apfs={IFS_APFS_MODULE_V1,sizeof(IfsApfsModuleV1),"apfs",
 IFS_APFS_CAP_RUN | IFS_APFS_CAP_FIXTURE,run,fixture};
const IfsApfsModuleV1 *infiltrator_filesystem_apfs_v1(void) {return &apfs;}
