#include <Ext2Srv.h>
#include <Sddl.h>
#include <Ext2Pipe.h>


/*
 * global defintions
 */

#define CL_ASSERT(cond) do {switch('x') {case (cond): case 0: break;}} while (0)

#define DEBUG(...)  do {} while(0)

/*
 * glboal variables
 */

BOOLEAN     g_stop = FALSE;

/* pipe handles */

PEXT2_PIPE  g_hep = NULL;

/*
 * function prototypes
 */

BOOLEAN Ext2QueryDrive (PPIPE_REQ *pr, ULONG len);
BOOLEAN Ext2DefineDrive(PPIPE_REQ *pr, ULONG len);
BOOLEAN Ext2RemoveDrive(PPIPE_REQ *pr, ULONG len);


/*
 * function body
 */

BOOL Ext2ReadPipe(HANDLE p, PVOID b, DWORD c, PDWORD r)
{
    DWORD bytes = 0, total = 0;
    BOOL  rc = FALSE;

    while (total < c) {
        rc = ReadFile(p,(PCHAR)b + total, c - total, &bytes, NULL);
        if (rc) {
            total += bytes;
        } else {
            break;
        }
    }

    if (r)
        *r = total;
    return rc;
}


BOOL Ext2WritePipe(HANDLE p, PVOID b, DWORD c, PDWORD w)
{
    DWORD bytes = 0, total = 0;
    BOOL  rc = FALSE;

    while (total < c) {
        rc = WriteFile(p, (PCHAR)b + total, c - total, &bytes, NULL);
        if (rc) {
            total += bytes;
        } else {
            break;
        }
    }

    if (w)
        *w = total;
    return rc;
}


PSECURITY_ATTRIBUTES Ext2CreateSA()
{
    PSECURITY_ATTRIBUTES sa = NULL;
    BOOL                 rc = FALSE;

    PSECURITY_DESCRIPTOR SD = NULL;
    LPCTSTR              SACL =  // ("S:(ML;;NW;;;LW)")
                                    _T("D:")                   // Discretionary ACL
                                    _T("A;OICI;GA;;;BG")     // Allow access to 
                                                             // built-in guests
                                    _T("A;OICI;GA;;;AN")     // Allow access to 
                                                             // anonymous logon	
                                    _T("A;OICI;GA;;;AU")     // Allow 
                                                             // read/write/execute 
                                                             // to authenticated 
                                                             // users
                                    _T("A;OICI;GA;;;BA");    // Allow full control 
                                                             // to administrators

                               //   _T("S:(ML;;NW;;;LW)D:(A;OICI;GA;;;S-1-1-0)");
                               //   _T("S:(ML;;NW;;;LW)D:(A;;0x12019f;;;WD)");

    /* convert */
    rc = ConvertStringSecurityDescriptorToSecurityDescriptor(
                            SACL, SDDL_REVISION_1, &SD, NULL);
    if (!rc) {
        goto errorout; 
    }

    // Initialize a security attributes structure.
    sa = (PSECURITY_ATTRIBUTES) LocalAlloc(LPTR, sizeof(SECURITY_ATTRIBUTES));
    if (NULL == sa) {
        goto errorout; 
    }
    sa->nLength = sizeof (SECURITY_ATTRIBUTES);
    sa->lpSecurityDescriptor = SD;
    sa->bInheritHandle = TRUE;

errorout:

    if (!sa) {
        if (SD) 
            LocalFree(SD);
    }

    return sa;
}

VOID Ext2FreeSA(PSECURITY_ATTRIBUTES sa)
{
	PACL ACL = NULL;

    if (NULL == sa)
        return;

    if (sa->lpSecurityDescriptor) {
	    LocalFree(sa->lpSecurityDescriptor);
    }

	LocalFree(sa);
}

PEXT2_PIPE
Ext2CreatePipe()
{
    PEXT2_PIPE  ap = NULL;
    LPSECURITY_ATTRIBUTES sa = NULL;

    ap = new EXT2_PIPE;
    if (!ap) {
        return NULL;
    }
    memset(ap, 0, sizeof(EXT2_PIPE));

    sa = Ext2CreateSA();

    ap->p = CreateNamedPipe( _T(EXT2_MGR_SRV), PIPE_ACCESS_DUPLEX |
                              FILE_FLAG_WRITE_THROUGH /* | ACCESS_SYSTEM_SECURITY */,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
                              PIPE_WAIT /* PIPE_REJECT_REMOTE_CLIENTS */ ,
                              PIPE_UNLIMITED_INSTANCES,
                              REQ_BODY_SIZE, REQ_BODY_SIZE,
                              6000, sa);
    if (INVALID_HANDLE_VALUE == ap->p) {
        DWORD le = GetLastError();
        if (le == ERROR_PIPE_BUSY) {
            WaitNamedPipe(_T(EXT2_MGR_SRV), 1000);
        } else {
        }
        delete ap;
        ap = NULL;
        goto errorout;
    }

    /* create new sync event */
    ap->e = CreateEvent(NULL, TRUE, TRUE, NULL);
    if (INVALID_HANDLE_VALUE == ap->e) {
        CloseHandle(ap->p);
        delete ap;
        ap = NULL;
        goto errorout;
    }

errorout:

    if (sa)
        Ext2FreeSA(sa);

    return ap;
}


VOID Ext2DestroyPipe(PEXT2_PIPE ap)
{
    if (ap->p && ap->p != INVALID_HANDLE_VALUE)
        CloseHandle(ap->p);

    if (ap->e && ap->e != INVALID_HANDLE_VALUE)
        CloseHandle(ap->e);

    delete ap;
}


BOOLEAN Ext2QueryDrive(PPIPE_REQ *pr, ULONG len)
{
    PPIPE_REQ       p;
    PREQ_QUERY_DRV  q;

    CHAR            devPath[] = "A:";
    DWORD           rc = FALSE;

    if (!pr || !*pr)
        goto errorout;

    p = *pr;
    q = (PREQ_QUERY_DRV)&p->data[0];

    devPath[0] = q->drive;
    q->type = GetDriveTypeA(devPath);

    if (q->type != DRIVE_NO_ROOT_DIR) {
        CHAR *s = &q->name[0];
        ULONG l = len - sizeof(PIPE_REQ) - sizeof(REQ_QUERY_DRV);
        rc = QueryDosDeviceA(devPath, s, l);
        if (rc) {
            q->result = 1;
            p->len = sizeof(PIPE_REQ) + sizeof(REQ_QUERY_DRV) + strlen(s) + 1;
        } else {
            q->result = 0;
        }
    } else {
        q->result = 1;
    }


errorout:

    return TRUE;    
}


BOOLEAN Ext2DefineDrive(PPIPE_REQ *pr, ULONG len)
{
    PPIPE_REQ       p;
    PREQ_DEFINE_DRV q;
    BOOLEAN         rc = 0;

    CHAR            dosPath[] = "A:\0";

    if (!pr || !*pr)
        goto errorout;

    p = *pr;
    q = (PREQ_DEFINE_DRV)&p->data[0];

    dosPath[0] = q->drive;
    rc = q->result = DefineDosDeviceA(q->flags, dosPath, &q->name[0]);

errorout:

    return rc;    
}

VOID Ext2NotifyDefineDrive(PPIPE_REQ pr)
{
    TCHAR  task[60];
    PREQ_DEFINE_DRV q;

    if (!pr)
        return;

    q = (PREQ_DEFINE_DRV)&pr->data[0];

    if (q->result) {
        _stprintf_s(task, 59, _T("/add %C"), q->drive);
        Ext2NotifyUser(task, q->pid);
    }
}


BOOLEAN Ext2RemoveDrive(PPIPE_REQ *pr, ULONG len)
{
    PPIPE_REQ       p;
    PREQ_REMOVE_DRV q;
    BOOLEAN         rc = 0;

    CHAR            dosPath[] = "A:\0";

    if (!pr || !*pr)
        goto errorout;

    p = *pr;
    q = (PREQ_REMOVE_DRV)&p->data[0];

    dosPath[0] = q->drive;
    rc = q->result = DefineDosDeviceA(q->flags, dosPath, &q->name[0]);

errorout:

    return rc;    
}

VOID Ext2NotifyRemoveDrive(PPIPE_REQ pr)
{
    TCHAR  task[60];
    PREQ_REMOVE_DRV q;

    if (!pr)
        return;

    q = (PREQ_REMOVE_DRV)&pr->data[0];

    if (q->result) {
        _stprintf_s(task, 59, _T("/del %C"), q->drive);
        Ext2NotifyUser(task, q->pid);
    }
}


DWORD Ext2StartPipeSrv()
{
    PEXT2_PIPE      ap = NULL;
    PPIPE_REQ       pr = NULL;
    PIPE_REQ        ac;
    DWORD           le;
    ULONG           len = 0;
    BOOL            rc;

    DEBUG("server mode is to start...\n");

    len = REQ_BODY_SIZE;
    pr = (PIPE_REQ *) new CHAR[len];
    if (!pr) {
        goto errorout;
    }

	while (true) {

        int         times = 0;

retry:

        if (g_stop)
            goto errorout;

        /* create named pipe */
        ap = Ext2CreatePipe();
        if (NULL == ap) {
            if (times++ < 10) {
                Sleep(250 * times);
                goto retry;
            }
            continue;
        }

        /* ASSD_PIPE is valid or not */
        if (!ap->p || ap->p == INVALID_HANDLE_VALUE ||
            !ap->e || ap->e == INVALID_HANDLE_VALUE) {
            Ext2DestroyPipe(ap);
            if (times++ < 10) {
                Sleep(500);
                goto retry;
            }
            continue;
        }

        g_hep = ap;

        /* wait connection request from client */
        memset(&ap->o, 0, sizeof(OVERLAPPED));
        ap->o.hEvent = ap->e;
        rc = ConnectNamedPipe(ap->p, &ap->o);
        le = GetLastError();
        if (rc == 0 && le == ERROR_PIPE_CONNECTED) {
            SetEvent(ap->e);
        }

        DEBUG("server mode started.\n");

        /* wait until client connects */
        WaitForSingleObject(ap->e, INFINITE);

        DEBUG("got client connected.\n");
			
		do {

			// wait for a command
			DWORD bytes=0;

            memset(&ac, 0, sizeof(ac));
			if (!Ext2ReadPipe(ap->p, &ac, sizeof(ac), &bytes)) {
				break;
			}

			if (ac.magic != PIPE_REQ_MAGIC || ac.len <= sizeof(ac)) {
				break;
            }

            if (ac.len > len) {
                if (pr)
                    delete [] pr;
                pr = (PPIPE_REQ) new CHAR[ac.len + 4];
                if (!pr) {
                    break;
                }
                len = ac.len + 4;
            }

            memset(pr, 0, len);
            memcpy(pr, &ac, sizeof(ac));
			if (!Ext2ReadPipe(ap->p, &pr->data[0],
                              ac.len - sizeof(ac), &bytes)) {
				break;
			}

            if (pr->cmd == CMD_QUERY_DRV) {
                DEBUG("got CMD_QUERY_DRV.\n");
                rc = Ext2QueryDrive(&pr, len);
            } else if (pr->cmd == CMD_DEFINE_DRV) {
                DEBUG("got CMD_DEFINE_DRV.\n");
                rc = Ext2DefineDrive(&pr, len);
            } else if (pr->cmd == CMD_REMOVE_DRV) {
                DEBUG("got CMD_REMOVE_DRV.\n");
                rc = Ext2RemoveDrive(&pr, len);
			} else {
                rc = FALSE;
                DEBUG("got unknown CMD.\n");
				break;
            }

            if (!Ext2WritePipe(ap->p, pr, pr->len, &bytes)) {
				break;
            }

            if (rc) {

                if (pr->cmd == CMD_REMOVE_DRV) {
                    Ext2NotifyRemoveDrive(pr);
                } else if (pr->cmd == CMD_DEFINE_DRV) {
                    Ext2NotifyDefineDrive(pr);
                }
            }

		} while (true);

        DEBUG("client disconnected.\n");

		Ext2DestroyPipe(ap);
        g_hep = NULL;

        DEBUG("Waiting for next client.\n");

    }

errorout:

	return 0;
}

VOID Ext2StopPipeSrv()
{
    g_stop = TRUE;

    if (g_hep)
        SetEvent(g_hep->e);
}
